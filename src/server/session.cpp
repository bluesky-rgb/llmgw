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

  // 2. 请求头：总时长不超过 read_timeout（防慢速攻击）
  http::request_parser<http::string_body> parser;
  parser.header_limit(kMaxHeaderBytes);
  parser.body_limit(cfg.max_body_bytes);
  timer.expires_after(cfg.read_timeout);
  auto h =
      co_await (http::async_read_header(socket_, buffer_, parser, kTuple) || timer_fired(&timer));
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
      co_await reject_and_close(
          make_error(ErrorCode::kPayloadTooLarge,
                     "请求体超过上限 " + std::to_string(cfg.max_body_bytes) + " 字节"),
          "too_large");
    } else {
      co_await reject_and_close(
          make_error(ErrorCode::kInvalidRequest, "无法解析的 HTTP 请求：" + ec.message()),
          "bad_request");
    }
    co_return ReadResult::kClose;
  }

  // 3. 请求体：不限总时长，只要求持续有进展（32 MB 的图片在慢速网络上要传几十秒，是正常请求）
  if (!parser.is_done()) {
    SteadyClock::time_point last_activity = SteadyClock::now();
    auto b = co_await (read_body(&parser, &last_activity) ||
                       idle_watch(&timer, &last_activity, cfg.read_timeout));
    if (b.index() == 1) {
      worker_->metrics->increment(Counter::kRejected, {"slow_request"});
      co_return ReadResult::kClose;
    }
    if (auto ec = std::get<0>(b); ec) {
      if (ec == http::error::body_limit) {
        co_await reject_and_close(
            make_error(ErrorCode::kPayloadTooLarge,
                       "请求体超过上限 " + std::to_string(cfg.max_body_bytes) + " 字节"),
            "too_large");
      }
      co_return ReadResult::kClose;
    }
  }
  *out = parser.release();
  co_return ReadResult::kOk;
}

Task<beast::error_code> Session::read_body(http::request_parser<http::string_body>* parser,
                                           SteadyClock::time_point* last_activity) {
  while (!parser->is_done()) {
    auto [ec, n] = co_await http::async_read_some(socket_, buffer_, *parser, kTuple);
    if (ec) co_return ec;
    *last_activity = SteadyClock::now();
  }
  co_return beast::error_code{};
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
                               bool keep_alive, unsigned version, std::optional<Error> error) {
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
  res.keep_alive(keep_alive);
  res.body() = std::move(body);
  res.prepare_payload();
  auto [ec, n] = co_await http::async_write(socket_, res, kTuple);
  co_return !ec;
}

Task<bool> Session::write_error(Error error, std::string request_id, bool keep_alive,
                                unsigned version) {
  const auto status = static_cast<unsigned>(http_status(error.code));
  std::string body = error_body(error);
  co_return co_await write_json(status, std::move(body), std::move(request_id), keep_alive, version,
                                std::move(error));
}

Task<> Session::reject_and_close(Error error, std::string reason_label) {
  worker_->metrics->increment(Counter::kRejected, {reason_label});
  co_await write_error(std::move(error), request_id({}, worker_->rng), false, 11);
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
  res.set("x-should-retry", "true");
  res.set(http::field::retry_after, "1");
  res.keep_alive(false);
  res.body() = error_body(error);
  res.prepare_payload();
  co_await http::async_write(socket, res, kTuple);
  beast::error_code ignored;
  socket.shutdown(tcp::socket::shutdown_both, ignored);
}

}  // namespace llmgw::server::detail
