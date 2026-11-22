// /v1/chat/completions：竞速 + 拉取循环（ADR-006 结论 1、2）。
//
//   handle_chat:  relay ‖ 客户端断开 ‖ 总截止时间     （每个请求一次）
//                 → 竞速结束之后：中间件的 on_complete（限时，不会被客户端断开取消；任务 2.1）
//   relay:        中间件的 on_request → dispatch() → (respond ‖ 看门狗)
//   respond:      写错误响应；或者写响应头 → 拉取循环
//
// 看门狗是惰性定时器，按当前所处的阶段分别计时（独立审查发现两者原来混在一起）：
//   等上游的下一块      超过 timeouts.idle        → 上游空闲超时
//   等客户端读走数据    超过 server.write_timeout → 客户端不读，断开以释放连接名额
//
// 竞速的分支绝不能抛异常：a || b 是 wait_for_one_success，抛异常的分支不会结束竞速，
// 而是被悄悄吞掉、一直等到别的分支“成功”。所以 relay 和 pump 在内部捕获全部异常。
#include <linux/sockios.h>
#include <sys/ioctl.h>

#include <algorithm>
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
  enum class Phase : std::uint8_t { kOther, kWaitingUpstream, kWritingClient };
  RequestContext* ctx = nullptr;
  middleware::Chain::Run* run =
      nullptr;  ///< 中间件链上的进度（与 ctx 一样在 handle_chat 的栈帧中）
  unsigned version = 11;
  bool keep_alive = true;
  bool headers_sent = false;  ///< 开始写响应之后，出错只能断开连接，不能再改状态码
  bool rejected_by_plugin = false;  ///< 中间件拒绝了请求（终态一律是 kRejected，不看状态码）
  Phase phase = Phase::kOther;
  SteadyClock::time_point phase_since;

  void enter(Phase p) {
    phase = p;
    phase_since = SteadyClock::now();
  }
  /// 写到一半时被打断：连接上的数据可能不完整，不能再追加任何东西
  [[nodiscard]] bool mid_write() const { return phase == Phase::kWritingClient; }
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
  const bool keep_alive = request.keep_alive() && request.version() == 11;
  auto fields = parse_chat_fields(&request.body());
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
  middleware::Chain::Run run;
  Exchange ex{.ctx = &ctx,
              .run = &run,
              .version = request.version(),
              .keep_alive = keep_alive,
              .headers_sent = false,
              .phase = Exchange::Phase::kOther,
              .phase_since = now};

  asio::steady_timer deadline(socket_.get_executor());
  deadline.expires_at(ctx.request.deadline);
  auto r = co_await (relay(&ex) || client_closed(&socket_) || timer_fired(&deadline));
  bool reuse = false;
  switch (r.index()) {
    case 0:
      reuse = std::get<0>(r);
      break;
    case 1:  // 客户端走了：dispatch 或拉取已被取消，上游请求随之结束
      set_outcome(&ctx, OutcomeKind::kClientAborted, ctx.outcome.http_status);
      break;
    default: {  // 总超时
      Error err =
          make_error(ErrorCode::kUpstreamTimeout,
                     "请求超过总超时 " + std::to_string(worker_->config->timeouts.total.count()) +
                         " 秒。上游可能已经开始生成并计费，请不要自动重试");
      if (!ex.headers_sent) {
        set_outcome(&ctx, OutcomeKind::kTimeout, 504, ErrorCode::kUpstreamTimeout);
        co_await write_error(std::move(err), ctx.request.request_id, false, ex.version,
                             run.response_headers);
      } else {
        set_outcome(&ctx, OutcomeKind::kTimeout, ctx.outcome.http_status,
                    ErrorCode::kUpstreamTimeout);
        co_await try_end_with_error_event(&ex, std::move(err));
      }
      break;
    }
  }
  // 无论竞速怎样结束，都要执行 on_complete（ctx.outcome 此时是终态）。没有中间件时连协程都不调用
  // （热路径上不多分配一个协程帧）
  bool completed = true;
  if (worker_->chain != nullptr && run.entered > 0) completed = co_await finish_chain(&ex);
  const bool keep_connection = reuse && completed;
  co_return keep_connection;
}

Task<bool> Session::finish_chain(Exchange* ex) {
  middleware::Chain* chain = worker_->chain;
  if (chain == nullptr || ex->run->entered == 0) co_return true;  // 没有中间件：不建定时器
  asio::steady_timer timer(socket_.get_executor());
  timer.expires_after(worker_->complete_timeout);
  // Chain::on_complete 捕获插件的全部异常，不会抛出（竞速的分支不能抛异常）
  auto r = co_await (chain->on_complete(ex->ctx, ex->run) || timer_fired(&timer));
  if (r.index() == 0) co_return true;
  const middleware::Chain::Run& run = *ex->run;
  const bool skipped = run.completed < run.entered;  // 有插件没有执行 on_complete
  const std::string plugin = run.cut_off ? chain->name(*run.cut_off) : std::string("unknown");
  std::fprintf(stderr,
               "[server] request %s: plugin %s on_complete did not finish within %lld ms%s\n",
               ex->ctx->request.request_id.c_str(), plugin.c_str(),
               static_cast<long long>(worker_->complete_timeout.count()),
               skipped ? "; skipped the remaining plugins and closed the connection" : "");
  worker_->metrics->increment(Counter::kMiddlewareErrors, {plugin, "complete_timeout"});
  co_return !skipped;
}

Task<bool> Session::relay(Exchange* ex) {
  RequestContext* ctx = ex->ctx;
  Result<std::unique_ptr<ResponseStream>> dispatched = fail(ErrorCode::kInternal, "网关内部错误");
  std::string thrown;  // catch 中不能 co_await：先记下来，之后再判断是不是因为被取消
  bool rejected = false;
  try {
    // 中间件（任务 2.1）：拒绝时不调用 dispatch，直接返回插件给出的错误。Chain 捕获插件的异常
    if (middleware::Chain* chain = worker_->chain; chain != nullptr) {
      middleware::Decision decision = co_await chain->on_request(ctx, ex->run);
      if (decision.reject) {
        dispatched = fail(std::move(*decision.reject));
        rejected = true;
        ex->rejected_by_plugin = !ex->run->failed;  // 插件抛异常：是故障（kFailed），不是拒绝
      }
    }
    if (!rejected) dispatched = co_await worker_->dispatcher->dispatch(ctx);
  } catch (const std::exception& e) {
    thrown = e.what();  // Dispatcher 的异常只影响这一个请求（异常隔离）
  }
  // 竞速中别的分支已经赢了（客户端断开、总超时）：dispatch 因被取消而返回，这里什么都不能再写，
  // 由赢的那个分支收尾（比如写 504）。否则会出现两份响应，或者 headers_sent 被置为 true 导致 504
  // 写不出去
  if (is_cancelled(co_await kCancellationState)) co_return false;
  if (!thrown.empty()) {
    std::fprintf(stderr, "[server] request %s: dispatch threw: %s\n",
                 ctx->request.request_id.c_str(), thrown.c_str());
  }

  asio::steady_timer timer(socket_.get_executor());
  auto r = co_await (respond(ex, std::move(dispatched)) || watchdog(ex, &timer));
  if (r.index() == 0) co_return std::get<0>(r);
  if (std::get<1>(r) == WatchdogFired::kUpstreamIdle) {
    set_outcome(ctx, OutcomeKind::kTimeout, ctx->outcome.http_status, ErrorCode::kUpstreamTimeout);
    co_await try_end_with_error_event(
        ex, make_error(ErrorCode::kUpstreamTimeout, "上游超过空闲超时没有输出"));
  } else {
    set_outcome(ctx, OutcomeKind::kClientAborted, ctx->outcome.http_status);  // 客户端长时间不读
  }
  co_return false;
}

Task<bool> Session::respond(Exchange* ex, Result<std::unique_ptr<ResponseStream>> dispatched) {
  RequestContext* ctx = ex->ctx;
  bool crashed = false;
  bool reuse = false;
  std::string thrown;
  try {
    if (!dispatched) {
      const Error& err = dispatched.error();
      const int status = http_status(err.code);
      // 中间件拒绝的（包括 503 等 5xx）是 kRejected；网关自己的 4xx（模型不存在等）也是：请求本身
      // 没有被处理。5xx 是故障
      const bool rejected = ex->rejected_by_plugin || status < 500;
      set_outcome(ctx, rejected ? OutcomeKind::kRejected : OutcomeKind::kFailed, status, err.code);
      ex->headers_sent = true;  // 开始写响应之后，总超时不能再写 504（否则两份响应会混在一起）
      ex->enter(Exchange::Phase::kWritingClient);
      const bool ok = co_await write_error(err, ctx->request.request_id, ex->keep_alive,
                                           ex->version, ex->run->response_headers);
      ex->enter(Exchange::Phase::kOther);
      co_return ok && ex->keep_alive;
    }
    std::unique_ptr<ResponseStream> stream = std::move(*dispatched);
    if (stream == nullptr) throw std::logic_error("Dispatcher 返回了空的 ResponseStream");
    const int status = stream->head().status;
    if (status < 100 || status > 599) {  // Beast 对超过 999 的状态码会抛异常；100–599 以外都不合法
      throw std::logic_error("Dispatcher 返回了不合法的状态码 " + std::to_string(status));
    }
    if (!co_await write_head(ex, &stream->head())) {
      set_outcome(ctx, OutcomeKind::kClientAborted, 0);
      co_return false;
    }
    // 注意不能写成 keep_alive && co_await pump(...)：短路求值会让 HTTP/1.0（keep_alive 为
    // false）根本不拉取
    const PumpResult pumped = co_await pump(ex, stream.get());
    reuse = ex->keep_alive && pumped == PumpResult::kDone;
  } catch (const std::exception& e) {
    crashed = true;
    thrown = e.what();
  }
  if (crashed && is_cancelled(co_await kCancellationState))
    co_return false;  // 被取消后 co_await 抛出的异常，不是错误
  if (crashed) {
    std::fprintf(stderr, "[server] request %s: respond threw: %s\n",
                 ctx->request.request_id.c_str(), thrown.c_str());
  }
  if (crashed &&
      !ex->headers_sent) {  // 还没开始写响应：返回 500（catch 中不能 co_await，所以放在这里）
    set_outcome(ctx, OutcomeKind::kFailed, 500, ErrorCode::kInternal);
    ex->headers_sent = true;
    co_await write_error(make_error(ErrorCode::kInternal, "网关内部错误"), ctx->request.request_id,
                         false, ex->version, ex->run->response_headers);
  } else if (crashed) {
    set_outcome(ctx, OutcomeKind::kFailed, ctx->outcome.http_status, ErrorCode::kInternal);
  }
  co_return reuse;
}

namespace {

// 发送队列中还没被对方确认的字节数（Linux 的 SIOCOUTQ）；出错返回 -1。
int unacked_bytes(tcp::socket* socket) {
  int queued = 0;
  return ::ioctl(socket->native_handle(), SIOCOUTQ, &queued) == 0 ? queued : -1;
}

}  // namespace

Task<Session::WatchdogFired> Session::watchdog(Exchange* ex, asio::steady_timer* timer) {
  // 选中的目标自己的空闲超时（executor 写入，任务 1.7），没有时用全局值
  const SteadyClock::duration idle =
      ex->ctx->upstream_idle.value_or(SteadyClock::duration(ex->ctx->config->timeouts.idle));
  const SteadyClock::duration write = worker_->config->server.write_timeout;
  // 每次最多睡 min(idle, write)：阶段可能在睡眠期间从“限时长的”变成“限时短的”（比如写响应头时
  // 睡了 60 秒，醒来前已经在等上游，而空闲超时只有 1 秒）。只按当前阶段的期限睡的话就会漏掉。
  // 不在每次阶段变化时重设定时器，是为了不在每个数据块上付出系统调用（ADR-006 结论 2）。
  const SteadyClock::duration check = std::min(idle, write);
  // 写客户端阶段的“进展”还要看内核：对方读走数据、确认之后，发送队列会变短。只看应用层的写入完成
  // 是不够的：Linux 要等发送缓冲区空出 1/3 才唤醒写入者，缓冲区自动增长到几 MB 时，读得慢的
  // 客户端会让一次写入阻塞好几秒，但数据一直在流动（任务 1.3 排查中实测）
  int last_unacked = -1;
  SteadyClock::time_point kernel_progress{};
  auto last_phase = Exchange::Phase::kOther;
  auto last_since = SteadyClock::time_point{};
  for (;;) {
    const auto phase = ex->phase;
    const auto since = ex->phase_since;
    if (phase != last_phase || since != last_since) {
      last_unacked = -1;  // 进入了新的阶段或有了新进展：重新开始观察内核的发送队列
      kernel_progress = {};
      last_phase = phase;
      last_since = since;
    }
    const auto effective = std::max(since, kernel_progress);
    const SteadyClock::duration limit = phase == Exchange::Phase::kWritingClient ? write : idle;
    const auto now = SteadyClock::now();
    timer->expires_at(phase == Exchange::Phase::kOther ? now + check
                                                       : std::min(effective + limit, now + check));
    if (auto [ec] = co_await timer->async_wait(kTuple); ec)
      co_return WatchdogFired::kUpstreamIdle;  // 被取消
    // 阶段变了、或者同一阶段内有了新进展（写出了一部分数据），都重新计时
    if (ex->phase != phase || ex->phase_since != since || phase == Exchange::Phase::kOther)
      continue;
    if (SteadyClock::now() - effective < limit) continue;
    if (phase == Exchange::Phase::kWritingClient) {
      const int unacked = unacked_bytes(&socket_);
      if (unacked >= 0 && (last_unacked < 0 || unacked < last_unacked)) {
        last_unacked = unacked;  // 发送队列在变短：客户端在读，只是慢
        kernel_progress = SteadyClock::now();
        continue;
      }
    }
    co_return phase == Exchange::Phase::kWritingClient ? WatchdogFired::kClientWrite
                                                       : WatchdogFired::kUpstreamIdle;
  }
}

Task<Session::PumpResult> Session::pump(Exchange* ex, ResponseStream* stream) {
  RequestContext* ctx = ex->ctx;
  const int status = stream->head().status;
  for (;;) {
    Result<std::optional<Chunk>> next = co_await pull(ex, stream);
    if (!next && is_cancelled(co_await kCancellationState))
      co_return PumpResult::kStreamError;  // 被取消：不是错误
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

Task<Result<std::optional<Chunk>>> Session::pull(Exchange* ex, ResponseStream* stream) {
  ex->enter(Exchange::Phase::kWaitingUpstream);
  Result<std::optional<Chunk>> next = fail(ErrorCode::kInternal, "网关内部错误");
  std::string thrown;
  try {
    next = co_await stream->next();
  } catch (const std::exception& e) {  // co_await 不能出现在 catch 块里：先记下，块外处理
    thrown = e.what();
  }
  ex->enter(Exchange::Phase::kOther);
  if (!thrown.empty() && !is_cancelled(co_await kCancellationState)) {
    std::fprintf(stderr, "[server] request %s: next() threw: %s\n",
                 ex->ctx->request.request_id.c_str(), thrown.c_str());
  }
  co_return next;
}

Task<bool> Session::write_head(Exchange* ex, const ResponseHead* head) {
  http::response<http::empty_body> res;
  res.version(ex->version);
  res.result(static_cast<unsigned>(head->status));
  for (const auto& h : head->headers) {
    if (is_hop_by_hop(h.name)) continue;
    if (!safe_header(h.name, h.value)) {
      // 不合法的响应头（含 CR/LF 等）：丢弃，响应照常返回。原样转发会让上游向客户端注入任意响应头
      std::fprintf(stderr, "[server] request %s: dropped unsafe upstream header\n",
                   ex->ctx->request.request_id.c_str());
      continue;
    }
    res.insert(h.name, h.value);
  }
  // 中间件附加的响应头：替换上游的同名响应头（比如两边都有 x-ratelimit-remaining
  // 时，以网关的为准）。
  // 先删掉上游的同名头，再逐个插入：同一个名字可以出现多次（Link、Server-Timing …）
  for (const Header& h : ex->run->response_headers) {
    if (plugin_header_allowed(h.name, h.value)) res.erase(h.name);
  }
  for (const Header& h : ex->run->response_headers) {
    if (plugin_header_allowed(h.name, h.value)) res.insert(h.name, h.value);
  }
  res.set("X-Request-Id", ex->ctx->request.request_id);
  res.keep_alive(ex->keep_alive);
  if (ex->version == 11) res.chunked(true);  // HTTP/1.0 不支持分块：响应体写完后关闭连接
  http::response_serializer<http::empty_body> sr{res};
  ex->headers_sent = true;
  ex->ctx->outcome.http_status = head->status;
  ex->enter(Exchange::Phase::kWritingClient);
  auto [ec, n] = co_await http::async_write_header(socket_, sr, kTuple);
  if (ec) co_return false;
  ex->enter(Exchange::Phase::kOther);
  co_return true;
}

namespace {

// 写出全部数据，每写出一部分就记一次进展（Exchange::phase_since）。写超时按“连续多久没有任何进展”
// 计算，而不是“多久没写完”：客户端一直在读、只是读得慢时，内核要攒够空间才通告接收窗口，
// 一次整块写入可能超过 write_timeout，但始终有进展（任务 1.3 排查中发现）。
// 正常情况下第一次 write_some 就全部写完，系统调用次数与 async_write 相同。
template <class Buffers>
Task<bool> write_with_progress(tcp::socket* socket, Buffers buffers,
                               SteadyClock::time_point* progress) {
  beast::buffers_suffix<Buffers> rest(buffers);
  while (beast::buffer_bytes(rest) > 0) {
    auto [ec, n] = co_await socket->async_write_some(rest, kTuple);
    if (ec) co_return false;
    rest.consume(n);
    *progress = SteadyClock::now();
  }
  co_return true;
}

}  // namespace

Task<bool> Session::write_chunk(Exchange* ex, std::string_view data) {
  if (data.empty()) co_return true;  // 空的分块会被当作“结束”，不能发送
  ex->enter(Exchange::Phase::kWritingClient);
  const bool ok = ex->version == 11
                      ? co_await write_with_progress(&socket_, http::make_chunk(asio::buffer(data)),
                                                     &ex->phase_since)
                      : co_await write_with_progress(
                            &socket_, asio::const_buffer(asio::buffer(data)), &ex->phase_since);
  if (!ok) co_return false;
  ex->enter(Exchange::Phase::kOther);
  co_return true;
}

Task<bool> Session::finish_body(Exchange* ex) {
  if (ex->version != 11) co_return true;
  ex->enter(Exchange::Phase::kWritingClient);
  if (!co_await write_with_progress(&socket_, http::make_chunk_last(), &ex->phase_since))
    co_return false;
  ex->enter(Exchange::Phase::kOther);
  co_return true;
}

Task<> Session::try_end_with_error_event(Exchange* ex, Error error) {
  // 只有流式、HTTP/1.1、并且没有写到一半的数据块时才能追加：否则会破坏分块的格式
  if (!ex->ctx->request.stream || ex->version != 11 || ex->mid_write()) co_return;
  asio::steady_timer timer(socket_.get_executor());
  timer.expires_after(std::chrono::seconds(1));  // 尽力而为：客户端不读时最多等 1 秒
  co_await (write_chunk(ex, error_event(error)) || timer_fired(&timer));
  if (ex->mid_write()) co_return;
  timer.expires_after(std::chrono::seconds(1));
  co_await (finish_body(ex) || timer_fired(&timer));
}

}  // namespace llmgw::server::detail
