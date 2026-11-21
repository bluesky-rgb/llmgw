// /v1/chat/completions：一次竞速 + 拉取循环（ADR-006 结论 1、2）。
//
//   handle_chat:  relay ‖ 客户端断开 ‖ 总截止时间          （每个请求一次）
//   relay:        dispatch() → 写响应头 → (pump ‖ 空闲超时)  （拿到响应头之后一次）
//
// 谁先结束就按谁处理，其余分支由 Asio 自动取消；取消沿着 co_await 链传到 Dispatcher 和
// ResponseStream。
#include <cstdio>
#include <exception>
#include <string>
#include <utility>

#include "http_util.h"
#include "session.h"

namespace llmgw::server::detail {

/// 一次请求-响应交换的状态。只存在于 handle_chat 的栈帧中；竞速的各个分支都引用它，
/// handle_chat 要等竞速结束才返回，所以引用一直有效。
struct Session::Exchange {
  RequestContext* ctx = nullptr;
  unsigned version = 11;
  bool keep_alive = true;
  bool headers_sent = false;  ///< 响应头发出之后，出错只能断开连接，不能再改状态码
  SteadyClock::time_point last_activity;
};

namespace {

Error make_error(ErrorCode code, std::string message) {
  return Error{.code = code, .message = std::move(message), .retry_after = {}};
}

void set_outcome(RequestContext* ctx, OutcomeKind kind, int status,
                 std::optional<ErrorCode> error = {}) {
  ctx->outcome = Outcome{.kind = kind, .http_status = status, .error = error};
}

}  // namespace

Task<bool> Session::handle_chat(Request request, std::string request_id) {
  auto fields = parse_chat_fields(&request.body());
  const bool keep_alive = request.keep_alive() && request.version() == 11;
  if (!fields) {
    co_await write_error(fields.error(), std::move(request_id), keep_alive, request.version());
    co_return keep_alive;
  }
  const auto now = SteadyClock::now();
  RequestContext ctx(RequestInfo{.request_id = std::move(request_id),
                                 .model = std::move(fields->model),
                                 .stream = fields->stream,
                                 .body = std::move(request.body()),
                                 .received_at = now,
                                 .deadline = now + worker_->config->timeouts.total},
                     worker_->config);
  Exchange ex{.ctx = &ctx,
              .version = request.version(),
              .keep_alive = keep_alive,
              .headers_sent = false,
              .last_activity = now};

  asio::steady_timer deadline(socket_.get_executor());
  deadline.expires_at(ctx.request.deadline);
  auto r = co_await (relay(&ex) || client_closed(&socket_) || timer_fired(&deadline));
  switch (r.index()) {
    case 0:
      co_return std::get<0>(r);
    case 1:  // 客户端走了：dispatch 或拉取已被取消，上游请求随之结束
      set_outcome(&ctx, OutcomeKind::kClientAborted, ctx.outcome.http_status);
      co_return false;
    default:  // 总超时
      if (!ex.headers_sent) {
        set_outcome(&ctx, OutcomeKind::kTimeout, 504, ErrorCode::kUpstreamTimeout);
        co_await write_error(
            make_error(ErrorCode::kUpstreamTimeout,
                       "请求超过总超时 " + std::to_string(worker_->config->timeouts.total.count()) +
                           " 秒。上游可能已经开始生成并计费，请不要自动重试"),
            ctx.request.request_id, false, ex.version);
      } else {
        set_outcome(&ctx, OutcomeKind::kTimeout, ctx.outcome.http_status,
                    ErrorCode::kUpstreamTimeout);
      }
      co_return false;
  }
}

Task<bool> Session::relay(Exchange* ex) {
  RequestContext* ctx = ex->ctx;
  Result<std::unique_ptr<ResponseStream>> dispatched = fail(ErrorCode::kInternal, "网关内部错误");
  try {
    dispatched = co_await worker_->dispatcher->dispatch(ctx);
  } catch (const std::exception& e) {
    // Dispatcher 的异常只影响这一个请求（异常隔离）
    std::fprintf(stderr, "[server] request %s: dispatch threw: %s\n",
                 ctx->request.request_id.c_str(), e.what());
  }
  // 竞速中别的分支已经赢了（客户端断开、总超时）：dispatch 因被取消而返回，这里什么都不能再写，
  // 由赢的那个分支收尾（比如写 504）。否则会出现两份响应，或者把 headers_sent 置为 true 导致 504
  // 写不出去
  if ((co_await asio::this_coro::cancellation_state).cancelled() != asio::cancellation_type::none) {
    co_return false;
  }
  if (!dispatched) {
    const Error& err = dispatched.error();
    const int status = http_status(err.code);
    set_outcome(ctx, status < 500 ? OutcomeKind::kRejected : OutcomeKind::kFailed, status,
                err.code);
    ex->headers_sent = true;  // 开始写响应之后，总超时不能再写 504（否则两份响应会混在一起）
    const bool ok = co_await write_error(err, ctx->request.request_id, ex->keep_alive, ex->version);
    co_return ok && ex->keep_alive;
  }
  std::unique_ptr<ResponseStream> stream = std::move(*dispatched);
  if (!co_await write_head(ex, &stream->head())) {
    set_outcome(ctx, OutcomeKind::kClientAborted, 0);
    co_return false;
  }

  // 拿到响应头之后：拉取循环 ‖ 空闲超时（两块数据之间的最长间隔）
  asio::steady_timer idle(socket_.get_executor());
  ex->last_activity = SteadyClock::now();
  auto r = co_await (pump(ex, stream.get()) ||
                     idle_watch(&idle, &ex->last_activity, ctx->config->timeouts.idle));
  if (r.index() == 1) {
    set_outcome(ctx, OutcomeKind::kTimeout, stream->head().status, ErrorCode::kUpstreamTimeout);
    co_return false;  // 响应头已经发出：只能断开
  }
  co_return ex->keep_alive&& std::get<0>(r) == PumpResult::kDone;
}

Task<Session::PumpResult> Session::pump(Exchange* ex, ResponseStream* stream) {
  RequestContext* ctx = ex->ctx;
  const int status = stream->head().status;
  for (;;) {
    Result<std::optional<Chunk>> next = co_await stream->next();
    if (!next) {
      // 上游中途出错：流式请求发送一条 SSE 错误事件后正常结束；非流式只能断开
      set_outcome(ctx, OutcomeKind::kFailed, status, next.error().code);
      if (!ctx->request.stream) co_return PumpResult::kStreamError;
      if (!co_await write_chunk(ex, error_event(next.error())) || !co_await finish_body(ex)) {
        co_return PumpResult::kClientError;
      }
      co_return ex->version == 11 ? PumpResult::kDone : PumpResult::kStreamError;
    }
    const std::optional<Chunk>& chunk = *next;
    if (!chunk) break;  // 正常结束
    ex->last_activity = SteadyClock::now();
    // 写完才拉下一块：客户端读得慢时在这里等待（背压）
    if (!co_await write_chunk(ex, chunk->data)) {
      set_outcome(ctx, OutcomeKind::kClientAborted, status);
      co_return PumpResult::kClientError;
    }
  }
  if (!co_await finish_body(ex)) {
    set_outcome(ctx, OutcomeKind::kClientAborted, status);
    co_return PumpResult::kClientError;
  }
  set_outcome(ctx, OutcomeKind::kSuccess, status);
  // HTTP/1.0：以关闭连接表示响应结束，所以不能复用
  co_return ex->version == 11 ? PumpResult::kDone : PumpResult::kStreamError;
}

Task<bool> Session::write_head(Exchange* ex, const ResponseHead* head) {
  http::response<http::empty_body> res;
  res.version(ex->version);
  res.result(static_cast<unsigned>(head->status));
  for (const auto& h : head->headers) {
    if (!is_hop_by_hop(h.name)) res.insert(h.name, h.value);
  }
  res.set("X-Request-Id", ex->ctx->request.request_id);
  res.keep_alive(ex->keep_alive);
  if (ex->version == 11) res.chunked(true);  // HTTP/1.0 不支持分块：响应体写完后关闭连接
  http::response_serializer<http::empty_body> sr{res};
  auto [ec, n] = co_await http::async_write_header(socket_, sr, kTuple);
  if (ec) co_return false;
  ex->headers_sent = true;
  ex->ctx->outcome.http_status = head->status;
  co_return true;
}

Task<bool> Session::write_chunk(const Exchange* ex, std::string_view data) {
  if (data.empty()) co_return true;  // 空的分块会被当作“结束”，不能发送
  if (ex->version == 11) {
    auto [ec, n] =
        co_await asio::async_write(socket_, http::make_chunk(asio::buffer(data)), kTuple);
    co_return !ec;
  }
  auto [ec, n] = co_await asio::async_write(socket_, asio::buffer(data), kTuple);
  co_return !ec;
}

Task<bool> Session::finish_body(const Exchange* ex) {
  if (ex->version != 11) co_return true;
  auto [ec, n] = co_await asio::async_write(socket_, http::make_chunk_last(), kTuple);
  co_return !ec;
}

}  // namespace llmgw::server::detail
