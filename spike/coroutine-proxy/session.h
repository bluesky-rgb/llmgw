// Session：每条客户端连接一个协程。
//
// 要验证的核心是 relay()：
//   一个请求只做一次四方竞速 ——
//     pump（读上游、写客户端的拉取循环） || 客户端断开 || 总超时 || 空闲超时
//   谁先结束就按谁处理，其余三个由 Asio 自动取消。
//   pump 写完客户端才读上游的下一块，所以客户端慢时上游也被压住（背压）。
//
// 早期版本在每个数据块上都做一次竞速，系统调用开销是搬数据本身的 8 倍以上，
// 见 docs/tasks/0.6-coroutine-demo.md 的验证记录。
#pragma once

#include <array>
#include <fstream>
#include <sstream>

#include "common.h"

namespace spike {

namespace detail {

using Clock = std::chrono::steady_clock;

inline std::int64_t rss_bytes() {
  std::ifstream f("/proc/self/status");
  std::string key;
  std::int64_t kb = 0;
  while (f >> key) {
    if (key == "VmRSS:") {
      f >> kb;
      return kb * 1024;
    }
    f.ignore(4096, '\n');
  }
  return 0;
}

// 等待客户端连接“可读”。等待响应期间，HTTP/1.1 客户端不会再发送数据，
// 所以一旦可读，几乎总是意味着对方关闭了连接。
// 已知限制：使用 HTTP 管线化（pipelining）的客户端会被误判为断开；
// 正式实现中用 MSG_PEEK 区分“有数据”和“已关闭”。
inline asio::awaitable<void> client_closed(tcp::socket& client) {
  co_await client.async_wait(tcp::socket::wait_read, kTuple);
}

inline asio::awaitable<void> timer_fired(asio::steady_timer& t) { co_await t.async_wait(kTuple); }

// 惰性空闲定时器：数据到达时只更新 last_activity，不重设定时器；
// 定时器到期时如果期间有过活动，就顺延到 last_activity + idle。
inline asio::awaitable<void> idle_watch(asio::steady_timer& t,
                                        const Clock::time_point& last_activity,
                                        Clock::duration idle) {
  for (;;) {
    t.expires_at(last_activity + idle);
    auto [ec] = co_await t.async_wait(kTuple);
    if (ec) co_return;  // 被取消：别的分支已经先结束了
    if (Clock::now() - last_activity >= idle) co_return;
  }
}

inline asio::awaitable<boost::system::error_code> write_simple(tcp::socket& client,
                                                               http::status status,
                                                               std::string body, bool keep_alive) {
  http::response<http::string_body> res{status, 11};
  res.set(http::field::content_type, "application/json");
  res.keep_alive(keep_alive);
  res.body() = std::move(body);
  res.prepare_payload();
  auto [ec, n] = co_await http::async_write(client, res, kTuple);
  co_return ec;
}

inline std::string error_json(std::string_view message, std::string_view code) {
  std::ostringstream o;
  o << R"({"error":{"message":")" << message << R"(","type":"gateway_error","code":")" << code
    << R"("}})";
  return o.str();
}

}  // namespace detail

class Session {
 public:
  // 参数按值传入：协程可能在调用者返回后继续运行，引用可能悬空
  Session(tcp::socket client, Options opt, tcp::resolver::results_type upstream, Stats* stats)
      : client_(std::move(client)),
        opt_(std::move(opt)),
        upstream_(std::move(upstream)),
        stats_(stats) {}

  asio::awaitable<void> run() {
    ++stats_->sessions_active;
    co_await serve();
    --stats_->sessions_active;
    boost::system::error_code ignored;
    client_.shutdown(tcp::socket::shutdown_both, ignored);
  }

 private:
  enum class PumpResult { kDone, kUpstreamError, kClientError };

  // 一次转发的状态。只在 relay() 的栈帧里存在，竞速的各个分支都引用它；
  // relay() 会等竞速结束才返回，所以引用一直有效。
  struct Relay {
    tcp::socket upstream;
    beast::flat_buffer buf;
    http::response_parser<http::buffer_body> parser;
    bool client_keep_alive = true;
    bool headers_sent = false;  // 响应头发出之后，出错只能断开连接，不能再改状态码
    detail::Clock::time_point last_activity = detail::Clock::now();
    std::array<char, 16 * 1024> chunk{};
    explicit Relay(const asio::any_io_executor& ex) : upstream(ex) {}
  };

  // Keep-Alive：同一条连接上依次处理多个请求
  asio::awaitable<void> serve() {
    beast::flat_buffer buf;
    asio::steady_timer idle(client_.get_executor());
    for (;;) {
      http::request_parser<http::string_body> parser;
      parser.header_limit(64 * 1024);
      parser.body_limit(32 * 1024 * 1024);
      idle.expires_after(opt_.idle_timeout);
      auto r =
          co_await (http::async_read(client_, buf, parser, kTuple) || detail::timer_fired(idle));
      if (r.index() == 1) co_return;                     // 空闲太久
      if (auto [ec, n] = std::get<0>(r); ec) co_return;  // 客户端关闭了连接，或请求不合法
      auto req = parser.release();
      ++stats_->requests;
      const bool keep_alive = req.keep_alive();

      if (req.target() == "/_proxy/stats") {
        if (co_await detail::write_simple(client_, http::status::ok, stats_json(), keep_alive)) {
          co_return;
        }
      } else if (!co_await relay(std::move(req))) {
        co_return;  // 响应没有正常完成，连接不能再复用
      }
      if (!keep_alive) co_return;
    }
  }

  // 把一个请求转发给上游，并把响应流式写回客户端。
  // 返回 true 表示响应完整写完，连接可以继续复用。
  asio::awaitable<bool> relay(http::request<http::string_body> req) {
    auto ex = client_.get_executor();
    Relay st(ex);
    st.client_keep_alive = req.keep_alive();
    st.parser.body_limit(boost::none);  // 流式响应没有长度上限
    asio::steady_timer deadline(ex);    // 整个请求的截止时间
    deadline.expires_after(opt_.total_timeout);
    asio::steady_timer idle(ex);  // 两次收到上游数据之间的最长间隔

    // 1. 连接并发送请求：只和截止时间竞速（此时还没有在等上游的响应）
    //    demo 中每个请求一条新连接；连接池在任务 1.4 中实现。
    req.set(http::field::host, opt_.upstream_host + ":" + opt_.upstream_port);
    req.keep_alive(false);
    req.prepare_payload();
    auto send = [&]() -> asio::awaitable<boost::system::error_code> {
      if (auto [ec, ep] = co_await asio::async_connect(st.upstream, upstream_, kTuple); ec) {
        co_return ec;
      }
      auto [ec, n] = co_await http::async_write(st.upstream, req, kTuple);
      co_return ec;
    };
    auto s = co_await (send() || detail::timer_fired(deadline));
    if (s.index() == 1 || std::get<0>(s)) {
      ++stats_->upstream_errors;
      const bool timeout = s.index() == 1;
      co_await detail::write_simple(
          client_, timeout ? http::status::gateway_timeout : http::status::bad_gateway,
          detail::error_json("upstream unavailable", "upstream_unavailable"), false);
      co_return false;
    }

    // 2. 唯一的一次竞速：拉取循环 || 客户端断开 || 总超时 || 空闲超时
    auto r =
        co_await (pump(st) || detail::client_closed(client_) || detail::timer_fired(deadline) ||
                  detail::idle_watch(idle, st.last_activity, opt_.idle_timeout));
    switch (r.index()) {
      case 0:
        switch (std::get<0>(r)) {
          case PumpResult::kDone:
            co_return true;
          case PumpResult::kClientError:
            ++stats_->client_aborted;
            ++stats_->upstream_cancelled;
            co_return false;
          case PumpResult::kUpstreamError:
            ++stats_->upstream_errors;
            if (!st.headers_sent) {
              co_await detail::write_simple(client_, http::status::bad_gateway,
                                            detail::error_json("upstream error", "upstream_error"),
                                            false);
            }
            co_return false;
        }
        co_return false;
      case 1:  // 客户端走了：pump 已被取消，st.upstream 析构时关闭上游连接
        ++stats_->client_aborted;
        ++stats_->upstream_cancelled;
        co_return false;
      default:  // 总超时或空闲超时
        ++stats_->timeouts;
        ++stats_->upstream_cancelled;
        if (!st.headers_sent) {  // 还没发过响应头，可以返回 504
          co_await detail::write_simple(client_, http::status::gateway_timeout,
                                        detail::error_json("upstream timeout", "upstream_timeout"),
                                        false);
        }
        co_return false;  // 响应头已经发出：只能断开
    }
  }

  // 拉取循环：先把缓冲区里已有的数据解析完（凑满最多 16KB 再写），缓冲区空了才读上游。
  asio::awaitable<PumpResult> pump(Relay& st) {
    // 2a. 上游响应头
    if (auto [ec, n] = co_await http::async_read_header(st.upstream, st.buf, st.parser, kTuple);
        ec) {
      co_return PumpResult::kUpstreamError;
    }
    st.last_activity = detail::Clock::now();

    // 2b. 把状态码和响应头写给客户端；响应体统一用分块传输，边收边发
    http::response<http::empty_body> head{st.parser.get().result(), 11};
    for (const auto& f : st.parser.get().base()) {
      if (f.name() != http::field::connection && f.name() != http::field::transfer_encoding &&
          f.name() != http::field::content_length) {
        head.set(f.name_string(), f.value());
      }
    }
    head.keep_alive(st.client_keep_alive);
    head.chunked(true);
    http::response_serializer<http::empty_body> sr{head};
    if (auto [ec, n] = co_await http::async_write_header(client_, sr, kTuple); ec) {
      co_return PumpResult::kClientError;
    }
    st.headers_sent = true;

    // 2c. 响应体
    // put() 每解析完一个上游分块就返回；把缓冲区里已有的分块都填进同一个 16KB 块，
    // 等“缓冲区读空 / 块写满 / 响应结束”时才写一次客户端。
    auto& body = st.parser.get().body();
    std::size_t filled = 0;
    while (!st.parser.is_done()) {
      body.data = st.chunk.data() + filled;
      body.size = st.chunk.size() - filled;
      beast::error_code ec;
      if (st.buf.size() > 0) st.buf.consume(st.parser.put(st.buf.data(), ec));
      const bool need_more = st.buf.size() == 0 || ec == http::error::need_more;
      if (ec == http::error::need_more || ec == http::error::need_buffer) ec = {};
      if (ec) co_return PumpResult::kUpstreamError;
      filled = st.chunk.size() - body.size;

      if (filled > 0 && (filled == st.chunk.size() || need_more || st.parser.is_done())) {
        // 客户端读得慢时在这里等待，不会去读上游（背压）
        auto [wec, wn] = co_await asio::async_write(
            client_, http::make_chunk(asio::buffer(st.chunk.data(), filled)), kTuple);
        if (wec) co_return PumpResult::kClientError;
        filled = 0;
      }
      if (!st.parser.is_done() && need_more) {
        auto [rec, rn] = co_await st.upstream.async_read_some(st.buf.prepare(16 * 1024), kTuple);
        if (rec) co_return PumpResult::kUpstreamError;  // 包括响应没结束上游就断开
        st.buf.commit(rn);
        st.last_activity = detail::Clock::now();
      }
    }
    if (auto [ec, n] = co_await asio::async_write(client_, http::make_chunk_last(), kTuple); ec) {
      co_return PumpResult::kClientError;
    }
    co_return PumpResult::kDone;
  }

  std::string stats_json() const {
    std::ostringstream o;
    o << R"({"sessions_active":)" << stats_->sessions_active.load() << R"(,"requests":)"
      << stats_->requests.load() << R"(,"client_aborted":)" << stats_->client_aborted.load()
      << R"(,"upstream_cancelled":)" << stats_->upstream_cancelled.load() << R"(,"timeouts":)"
      << stats_->timeouts.load() << R"(,"upstream_errors":)" << stats_->upstream_errors.load()
      << R"(,"loop_lag_max_us":)" << stats_->loop_lag_max_us.load() << R"(,"loop_lag_over_5ms":)"
      << stats_->loop_lag_over_5ms.load() << R"(,"loop_lag_over_20ms":)"
      << stats_->loop_lag_over_20ms.load() << R"(,"rss_bytes":)" << detail::rss_bytes() << "}";
    return o.str();
  }

  tcp::socket client_;
  Options opt_;
  tcp::resolver::results_type upstream_;
  Stats* stats_;  // 全局对象，生命周期长于所有会话
};

}  // namespace spike
