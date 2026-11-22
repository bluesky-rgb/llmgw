// Session：Keep-Alive 循环、读请求（大小上限与读取超时）、路由、错误响应。
#include "session.h"

#include <cstdio>
#include <exception>
#include <string>
#include <utility>

#include "http_util.h"

namespace llmgw::server::detail {
namespace {

constexpr auto kDrainTime = std::chrono::seconds(1);  // 回复错误后，丢弃对方剩余数据的最长时间

Error make_error(ErrorCode code, std::string message) {
  return Error{.code = code, .message = std::move(message), .retry_after = {}};
}

}  // namespace

Session::Session(tcp::socket socket, WorkerContext* worker)
    : socket_(std::move(socket)), worker_(worker), guard_(worker->shared) {}

Task<> run_session(std::unique_ptr<Session> session) { co_await session->run(); }

Task<> Session::run() {
  for (;;) {
    std::optional<Request> request;
    if (co_await read_request(&request) == ReadResult::kClose) break;
    if (!co_await handle(std::move(*request))) break;
  }
  beast::error_code ignored;
  socket_.shutdown(tcp::socket::shutdown_both, ignored);
}

Task<Session::ReadResult> Session::read_request(std::optional<Request>* out) {
  const auto& cfg = worker_->config->server;
  asio::steady_timer timer(socket_.get_executor());

  // 1. 等待下一个请求（长连接空闲）。缓冲区里已经有数据（管线化）时不用等。
  if (buffer_.size() == 0) {
    timer.expires_after(cfg.keep_alive_timeout);
    auto r = co_await (socket_.async_wait(tcp::socket::wait_read, kTuple) || timer_fired(&timer));
    if (r.index() == 1 || std::get<0>(std::get<0>(r))) co_return ReadResult::kClose;
  }

  Parser parser;
  parser.header_limit(kMaxHeaderBytes);
  parser.body_limit(cfg.max_body_bytes);
  if (co_await read_header_part(&parser, &timer) == ReadResult::kClose)
    co_return ReadResult::kClose;
  if (!parser.is_done() && co_await read_body_part(&parser, &timer) == ReadResult::kClose) {
    co_return ReadResult::kClose;
  }
  *out = parser.release();
  co_return ReadResult::kOk;
}

Task<Session::ReadResult> Session::read_header_part(Parser* parser_ptr, asio::steady_timer* timer) {
  const auto& cfg = worker_->config->server;
  Parser& parser = *parser_ptr;
  // 2. 请求头：总时长不超过 read_timeout（防慢速攻击）
  timer->expires_after(cfg.read_timeout);
  auto h =
      co_await (http::async_read_header(socket_, buffer_, parser, kTuple) || timer_fired(timer));
  if (h.index() == 1) {
    worker_->metrics->increment(Counter::kRejected, {"slow_request"});
    co_return ReadResult::kClose;  // 慢速攻击：直接关闭，不回复
  }
  if (auto [ec, n] = std::get<0>(h); ec) {
    if (ec == http::error::end_of_stream || ec == asio::error::eof ||
        ec == asio::error::connection_reset) {
      co_return ReadResult::kClose;
    }
    if (ec == http::error::header_limit) {
      co_await reject_and_close(make_error(ErrorCode::kHeadersTooLarge, "请求头超过 64 KB"),
                                "headers_too_large");
    } else if (ec == http::error::body_limit) {
      // 请求头已经解析完：沿用客户端的请求 ID 和 HTTP 版本
      co_await reject_and_close(
          make_error(ErrorCode::kPayloadTooLarge,
                     "请求体超过上限 " + std::to_string(cfg.max_body_bytes) + " 字节"),
          "too_large", std::string(parser.get()["X-Request-Id"]), parser.get().version());
    } else {
      co_await reject_and_close(
          make_error(ErrorCode::kInvalidRequest, "无法解析的 HTTP 请求：" + ec.message()),
          "bad_request");
    }
    co_return ReadResult::kClose;
  }

  // 分帧必须没有歧义（防请求走私）：Content-Length 与 Transfer-Encoding 同时出现、多个不同的
  // Content-Length，Beast 已经拒绝；这里再拒绝 chunked 以外的任何传输编码
  if (const auto te = parser.get()[http::field::transfer_encoding];
      !te.empty() && !is_plain_chunked(te)) {
    co_await reject_and_close(
        make_error(ErrorCode::kInvalidRequest,
                   "不支持的 Transfer-Encoding：" + std::string(te) + "（只支持 chunked）"),
        "bad_request", std::string(parser.get()["X-Request-Id"]), parser.get().version());
    co_return ReadResult::kClose;
  }
  co_return ReadResult::kOk;
}

Task<Session::ReadResult> Session::read_body_part(Parser* parser_ptr, asio::steady_timer* timer) {
  const auto& cfg = worker_->config->server;
  Parser& parser = *parser_ptr;
  // 3. 请求体：不限总时长（32 MB 的图片在慢速网络上要传几十秒，是正常请求），但要求持续有进展，
  //    并且过了宽限期之后平均速度不低于 kMinBodyBytesPerSecond（否则每 9 秒发 1
  //    个字节就能一直占住连接）
  SteadyClock::time_point last_activity = SteadyClock::now();
  auto b =
      co_await (read_body(&parser, &last_activity) || body_watch(timer, &parser, &last_activity));
  if (b.index() == 1) {
    worker_->metrics->increment(Counter::kRejected, {"slow_request"});
    co_return ReadResult::kClose;
  }
  if (auto ec = std::get<0>(b); ec) {
    const std::string id(parser.get()["X-Request-Id"]);
    if (ec == http::error::body_limit) {
      co_await reject_and_close(
          make_error(ErrorCode::kPayloadTooLarge,
                     "请求体超过上限 " + std::to_string(cfg.max_body_bytes) + " 字节"),
          "too_large", id, parser.get().version());
    } else if (ec != http::error::partial_message && ec != asio::error::eof &&
               ec != asio::error::connection_reset) {
      // 畸形的分块编码等：回复 400（客户端断开则不回复）
      co_await reject_and_close(
          make_error(ErrorCode::kInvalidRequest, "无法解析的请求体：" + ec.message()),
          "bad_request", id, parser.get().version());
    }
    co_return ReadResult::kClose;
  }
  co_return ReadResult::kOk;
}

Task<beast::error_code> Session::read_body(Parser* parser, SteadyClock::time_point* last_activity) {
  while (!parser->is_done()) {
    auto [ec, n] = co_await http::async_read_some(socket_, buffer_, *parser, kTuple);
    if (ec) co_return ec;
    *last_activity = SteadyClock::now();
  }
  co_return beast::error_code{};
}

Task<> Session::body_watch(asio::steady_timer* timer, const Parser* parser,
                           const SteadyClock::time_point* last_activity) {
  const auto grace = SteadyClock::duration(worker_->config->server.read_timeout);
  const auto start = SteadyClock::now();
  for (;;) {
    // 每秒检查一次（只在读请求体期间，每个请求一个定时器，不在热路径上）
    timer->expires_after(std::chrono::seconds(1));
    if (auto [ec] = co_await timer->async_wait(kTuple); ec) co_return;
    const auto now = SteadyClock::now();
    if (now - *last_activity >= grace) co_return;  // 连续 read_timeout 没有任何数据
    const double seconds = std::chrono::duration<double>(now - start - grace).count();
    if (seconds > 0 &&
        static_cast<double>(parser->get().body().size()) < kMinBodyBytesPerSecond * seconds) {
      co_return;  // 过了宽限期之后，平均速度太慢
    }
  }
}

Task<bool> Session::handle(Request request) {
  const std::string id = request_id(request["X-Request-Id"], worker_->rng);
  const bool keep_alive = request.keep_alive() && request.version() == 11;
  const unsigned version = request.version();
  const auto path = path_of(request.target());

  if (path == "/v1/chat/completions") {
    if (request.method() != http::verb::post) {
      co_await write_error(
          make_error(ErrorCode::kMethodNotAllowed, "/v1/chat/completions 只接受 POST"), id,
          keep_alive, version);
      co_return keep_alive;
    }
    co_return co_await handle_chat(std::move(request), id);
  }
  if (path == "/v1/models") {
    if (request.method() != http::verb::get) {
      co_await write_error(make_error(ErrorCode::kMethodNotAllowed, "/v1/models 只接受 GET"), id,
                           keep_alive, version);
      co_return keep_alive;
    }
    co_return co_await write_json(200, models_body(*worker_->config), id, keep_alive, version) &&
        keep_alive;
  }
  co_await write_error(
      make_error(ErrorCode::kNotFound, "路径不存在：" + std::string(path) +
                                           "（数据面提供 /v1/chat/completions 和 /v1/models）"),
      id, keep_alive, version);
  co_return keep_alive;
}

Task<bool> Session::write_json(unsigned status, std::string body, std::string request_id,
                               bool keep_alive, unsigned version, std::optional<Error> error,
                               std::vector<Header> extra) {
  http::response<http::string_body> res{static_cast<http::status>(status), version};
  res.set(http::field::content_type, "application/json");
  res.set("X-Request-Id", request_id);
  if (error) {
    res.set("x-should-retry", should_retry(error->code) ? "true" : "false");
    if (error->retry_after) {
      res.set(http::field::retry_after, std::to_string(error->retry_after->count()));
      res.set("retry-after-ms", std::to_string(error->retry_after->count() * 1000));
    }
    if (error->code == ErrorCode::kMethodNotAllowed) {
      res.set(http::field::allow, "POST, GET");
    }
  }
  for (const Header& h : extra) {  // 网关自己的头都不允许被插件替换，所以直接插入
    if (plugin_header_allowed(h.name, h.value)) res.insert(h.name, h.value);
  }
  res.keep_alive(keep_alive);
  res.body() = std::move(body);
  res.prepare_payload();
  // 与 write_timeout 竞速：客户端不读时，不能永远占住连接名额
  asio::steady_timer timer(socket_.get_executor());
  timer.expires_after(worker_->config->server.write_timeout);
  auto r = co_await (http::async_write(socket_, res, kTuple) || timer_fired(&timer));
  co_return r.index() == 0 && !std::get<0>(std::get<0>(r));
}

Task<bool> Session::write_error(Error error, std::string request_id, bool keep_alive,
                                unsigned version, std::vector<Header> extra) {
  const auto status = static_cast<unsigned>(http_status(error.code));
  std::string body = error_body(error);
  co_return co_await write_json(status, std::move(body), std::move(request_id), keep_alive, version,
                                std::move(error), std::move(extra));
}

Task<> Session::reject_and_close(Error error, std::string reason_label, std::string request_id,
                                 unsigned version) {
  worker_->metrics->increment(Counter::kRejected, {reason_label});
  co_await write_error(std::move(error), detail::request_id(request_id, worker_->rng), false,
                       version == 10 ? 10 : 11);
  beast::error_code ignored;
  socket_.shutdown(tcp::socket::shutdown_send, ignored);
  // 丢弃对方还在发送的数据（最多 kDrainTime），让对方先读到我们的回复
  asio::steady_timer timer(socket_.get_executor());
  timer.expires_after(kDrainTime);
  std::array<char, 4096> sink{};
  auto drain = [](tcp::socket* s, std::array<char, 4096>* buf) -> Task<> {
    for (;;) {
      auto [ec, n] = co_await s->async_read_some(asio::buffer(*buf), kTuple);
      if (ec) co_return;
    }
  };
  co_await (drain(&socket_, &sink) || timer_fired(&timer));
}

Task<> reject_overloaded(tcp::socket socket, WorkerContext* worker) {
  ConnectionGuard guard(worker->shared);  // 调用者已经计入连接数
  worker->metrics->increment(Counter::kRejected, {"overload"});
  const Error error{.code = ErrorCode::kOverloaded,
                    .message = "网关连接数已达上限",
                    .retry_after = std::chrono::seconds(1)};
  http::response<http::string_body> res{http::status::service_unavailable, 11};
  res.set(http::field::content_type, "application/json");
  res.set("X-Request-Id", request_id({}, worker->rng));
  res.set("x-should-retry", "true");
  res.set(http::field::retry_after, "1");
  res.set("retry-after-ms", "1000");
  res.keep_alive(false);
  res.body() = error_body(error);
  res.prepare_payload();
  asio::steady_timer timer(socket.get_executor());  // 客户端不读时最多等 write_timeout
  timer.expires_after(worker->config->server.write_timeout);
  co_await (http::async_write(socket, res, kTuple) || timer_fired(&timer));
  beast::error_code ignored;
  socket.shutdown(tcp::socket::shutdown_both, ignored);
}

}  // namespace llmgw::server::detail
