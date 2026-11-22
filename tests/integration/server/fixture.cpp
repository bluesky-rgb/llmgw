#include "fixture.h"

#include <deque>
#include <string_view>

namespace llmgw::server::test {
namespace {

Error make_error(ErrorCode code, std::string message,
                 std::optional<std::chrono::seconds> retry_after = {}) {
  return Error{.code = code, .message = std::move(message), .retry_after = retry_after};
}

// 按脚本输出数据块：每块之前等待 interval；脚本结束后可以出错、正常结束或一直等待
class FakeStream final : public ResponseStream {
 public:
  enum class End : std::uint8_t { kOk, kError, kHang, kThrow };
  FakeStream(ResponseHead head, std::deque<std::string> chunks, std::chrono::milliseconds interval,
             End end, std::shared_ptr<Probe> probe)
      : head_(std::move(head)),
        chunks_(std::move(chunks)),
        interval_(interval),
        end_(end),
        probe_(std::move(probe)) {}
  FakeStream(const FakeStream&) = delete;
  FakeStream& operator=(const FakeStream&) = delete;
  FakeStream(FakeStream&&) = delete;
  FakeStream& operator=(FakeStream&&) = delete;
  ~FakeStream() override {
    ++probe_->streams_destroyed;
    probe_->mark_cancel();
  }

  [[nodiscard]] const ResponseHead& head() const override { return head_; }

  Task<Result<std::optional<Chunk>>> next() override {
    asio::steady_timer t(co_await asio::this_coro::executor);
    if (chunks_.empty()) {
      if (end_ == End::kError)
        co_return fail(make_error(ErrorCode::kUpstreamUnavailable, "upstream closed"));
      if (end_ == End::kThrow) throw std::runtime_error("next() boom");
      if (end_ == End::kHang) {
        t.expires_at(asio::steady_timer::time_point::max());
        co_await t.async_wait(kTuple);
      }
      co_return std::optional<Chunk>{};
    }
    if (interval_.count() > 0) {
      t.expires_after(interval_);
      co_await t.async_wait(kTuple);
    }
    ++probe_->chunks_served;
    std::string data = std::move(chunks_.front());
    chunks_.pop_front();
    co_return std::optional<Chunk>(Chunk{std::move(data)});
  }

 private:
  ResponseHead head_;
  std::deque<std::string> chunks_;
  std::chrono::milliseconds interval_;
  End end_;
  std::shared_ptr<Probe> probe_;
};

std::deque<std::string> sse(int n, std::size_t size = 0) {
  std::deque<std::string> out;
  for (int i = 0; i < n; ++i) {
    out.push_back(size == 0 ? "data: " + std::to_string(i) + "\n\n" : std::string(size, 'x'));
  }
  return out;
}

}  // namespace

Task<Result<std::unique_ptr<ResponseStream>>> FakeDispatcher::dispatch(RequestContext* ctx) {
  ++probe_->dispatches;
  const std::string& m = ctx->request.model;
  ResponseHead head{.status = 200,
                    .headers = {{"content-type", "text/event-stream"}, {"x-upstream", "fake"}}};
  using End = FakeStream::End;
  auto make = [&](std::deque<std::string> chunks, std::chrono::milliseconds interval, End end) {
    return std::unique_ptr<ResponseStream>(
        std::make_unique<FakeStream>(head, std::move(chunks), interval, end, probe_));
  };
  if (m == "ok") co_return make(sse(3), std::chrono::milliseconds(0), End::kOk);
  if (m == "slow") co_return make(sse(1000), std::chrono::milliseconds(50), End::kOk);
  if (m == "stall") co_return make(sse(1), std::chrono::milliseconds(0), End::kHang);
  if (m == "stall_short_idle") {  // 任务 1.7：选中的目标自己的空闲超时（比全局的 1 秒短）
    ctx->upstream_idle = std::chrono::milliseconds(300);
    co_return make(sse(1), std::chrono::milliseconds(0), End::kHang);
  }
  if (m == "midfail") co_return make(sse(2), std::chrono::milliseconds(0), End::kError);
  if (m == "big") co_return make(sse(200, 64 * 1024), std::chrono::milliseconds(0), End::kOk);
  if (m == "inject") {  // 上游返回带 CR/LF 的响应头：不能被原样转发给客户端（响应头注入）
    head.headers.push_back({"x-evil", "a\r\nSet-Cookie: pwned=1"});
    head.headers.push_back({"x-bad name", "v"});
    co_return make(sse(1), std::chrono::milliseconds(0), End::kOk);
  }
  if (m == "medium") co_return make(sse(5), std::chrono::milliseconds(50), End::kOk);
  if (m == "nextthrow") co_return make(sse(1), std::chrono::milliseconds(0), End::kThrow);
  if (m == "badstatus") {
    head.status = 1000;
    co_return make(sse(1), std::chrono::milliseconds(0), End::kOk);
  }
  if (m == "worker") {
    head.headers.push_back({"x-worker", std::to_string(info_.index)});
    head.headers.push_back({"x-worker-count", std::to_string(info_.worker_count)});
    co_return make(sse(1), std::chrono::milliseconds(0), End::kOk);
  }
  if (m == "hang") {
    asio::steady_timer t(info_.executor, asio::steady_timer::time_point::max());
    co_await t.async_wait(kTuple);  // 只会因为被取消而结束
    ++probe_->dispatch_cancelled;
    probe_->mark_cancel();
    co_return fail(make_error(ErrorCode::kInternal, "cancelled"));
  }
  if (m == "throw") throw std::runtime_error("boom");
  if (m.starts_with("error:")) {
    const auto code = static_cast<ErrorCode>(std::stoi(m.substr(6)));
    co_return fail(make_error(code, "fake error", std::chrono::seconds(2)));
  }
  co_return fail(make_error(ErrorCode::kModelNotFound, "模型 \"" + m + "\" 没有配置"));
}

std::shared_ptr<config::Snapshot> test_config() {
  auto cfg = std::make_shared<config::Snapshot>();
  cfg->server.host = "127.0.0.1";
  cfg->server.port = 0;
  cfg->server.admin = {.host = "127.0.0.1", .port = 0};
  cfg->server.observability = {.host = "127.0.0.1", .port = 0};
  cfg->server.workers = 2;
  cfg->server.max_connections = 100;
  cfg->server.max_body_bytes = 64 * 1024;
  cfg->server.read_timeout = std::chrono::seconds(1);
  cfg->server.keep_alive_timeout = std::chrono::seconds(1);
  cfg->server.write_timeout = std::chrono::seconds(1);
  cfg->timeouts.idle = std::chrono::seconds(1);
  cfg->timeouts.total = std::chrono::seconds(2);
  cfg->models.push_back({.name = "smart", .targets = {}});
  cfg->models.push_back({.name = "fast", .targets = {}});
  return cfg;
}

void ServerTest::start(std::shared_ptr<config::Snapshot> cfg) {
  server_.reset();
  auto probe = probe_;
  server_ = std::make_unique<Server>(ServerOptions{
      .config = std::move(cfg),
      .dispatcher_factory =
          [probe](const WorkerInfo& w) { return std::make_unique<FakeDispatcher>(w, probe); },
      .metrics_factory = {}});
  auto r = server_->start();
  ASSERT_TRUE(r) << r.error().message;
}

Client::Client(std::uint16_t port, std::chrono::milliseconds timeout)
    : socket_(ioc_), timeout_(timeout) {
  socket_.connect(tcp::endpoint(asio::ip::make_address("127.0.0.1"), port));
}

template <class Start>
beast::error_code Client::run(Start start, std::chrono::milliseconds timeout) {
  beast::error_code result = asio::error::timed_out;
  bool done = false;
  start([&](beast::error_code ec, std::size_t /*n*/) {
    result = ec;
    done = true;
  });
  ioc_.restart();
  ioc_.run_for(timeout);
  if (!done) {
    beast::error_code ignored;
    socket_.cancel(ignored);
    ioc_.restart();
    ioc_.run();  // 让被取消的操作完成
    return asio::error::timed_out;
  }
  return result;
}

void Client::send(const std::string& raw) {
  auto ec =
      run([&](auto handler) { asio::async_write(socket_, asio::buffer(raw), handler); }, timeout_);
  ASSERT_FALSE(ec) << "发送失败：" << ec.message();
}

bool Client::try_send(const std::string& raw) {
  return !run([&](auto handler) { asio::async_write(socket_, asio::buffer(raw), handler); },
              timeout_);
}

std::string Client::read_some(std::size_t max) {
  if (buffer_.size() > 0) {
    const auto n = std::min(max, buffer_.size());
    std::string out(static_cast<const char*>(buffer_.data().data()), n);
    buffer_.consume(n);
    return out;
  }
  std::string out(max, '\0');
  std::size_t got = 0;
  auto ec = run(
      [&](auto handler) {
        socket_.async_read_some(asio::buffer(out),
                                [&, handler](beast::error_code e, std::size_t n) mutable {
                                  got = n;
                                  handler(e, n);
                                });
      },
      timeout_);
  out.resize(ec ? 0 : got);
  return out;
}

std::string Client::read_until_close_or(std::chrono::milliseconds within) {
  std::string out(static_cast<const char*>(buffer_.data().data()), buffer_.size());
  buffer_.consume(buffer_.size());
  const auto deadline = Clock::now() + within;
  std::array<char, 8192> buf{};
  for (;;) {
    const auto left =
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now());
    if (left.count() <= 0) return out;
    std::size_t got = 0;
    auto ec = run(
        [&](auto handler) {
          socket_.async_read_some(asio::buffer(buf),
                                  [&, handler](beast::error_code e, std::size_t n) mutable {
                                    got = n;
                                    handler(e, n);
                                  });
        },
        left);
    out.append(buf.data(), got);
    if (ec) return out;
  }
}

void Client::post_chat(const std::string& model, bool stream, const std::string& extra_headers) {
  const std::string body =
      R"({"model":")" + model + R"(","stream":)" + (stream ? "true" : "false") + "}";
  send("POST /v1/chat/completions HTTP/1.1\r\nHost: t\r\nContent-Type: application/json\r\n" +
       extra_headers + "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body);
}

Client::Response Client::read_response() {
  Response r;
  auto ec =
      run([&](auto handler) { http::async_read(socket_, buffer_, r.msg, handler); }, timeout_);
  r.ok = !ec;
  return r;
}

bool Client::read_head(http::response_parser<http::string_body>* parser) {
  return !run([&](auto handler) { http::async_read_header(socket_, buffer_, *parser, handler); },
              timeout_);
}

std::string Client::read_until_close() {
  std::string out(static_cast<const char*>(buffer_.data().data()), buffer_.size());
  buffer_.consume(buffer_.size());
  std::array<char, 8192> buf{};
  for (;;) {
    std::size_t got = 0;
    auto ec = run(
        [&](auto handler) {
          socket_.async_read_some(asio::buffer(buf),
                                  [&, handler](beast::error_code e, std::size_t n) mutable {
                                    got = n;
                                    handler(e, n);
                                  });
        },
        timeout_);
    out.append(buf.data(), got);
    if (ec) return out;
  }
}

bool Client::closed_within(std::chrono::milliseconds within) {
  const auto deadline = Clock::now() + within;
  std::array<char, 8192> buf{};
  for (;;) {
    const auto left =
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now());
    if (left.count() <= 0) return false;
    auto ec = run([&](auto handler) { socket_.async_read_some(asio::buffer(buf), handler); }, left);
    if (ec == asio::error::timed_out) return false;
    if (ec) return true;  // EOF、连接被重置
  }
}

void Client::close() {
  beast::error_code ignored;
  socket_.close(ignored);
}

}  // namespace llmgw::server::test
