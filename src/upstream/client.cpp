// Client：两个阶段各做一次竞速（ADR-006：竞速放在尝试的粒度上）。
//   连接阶段（deadlines.connect）：排队 → 复用前检查 / 新建连接（DNS、TCP、代理、TLS）
//   交换阶段（deadlines.first_byte）：写请求（复用的连接写失败时换新连接重发一次）→ 等响应头
// 响应体由 Body 读取，空闲超时由调用者（Session 的看门狗）负责。
#include "llmgw/upstream/client.h"

#include <algorithm>
#include <array>
#include <boost/asio/as_tuple.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/experimental/awaitable_operators.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/beast/http.hpp>
#include <cstdio>
#include <exception>
#include <map>
#include <utility>

#include "establish.h"
#include "message.h"
#include "pool.h"
#include "tls.h"

namespace llmgw::upstream {

namespace asio = boost::asio;
namespace http = boost::beast::http;
using namespace asio::experimental::awaitable_operators;  // NOLINT(google-build-using-namespace)
using detail::Connection;
using detail::Lease;
using detail::Pool;
using detail::Route;
constexpr auto kTuple = asio::as_tuple(asio::use_awaitable);

std::string_view stage_name(Stage stage) {
  switch (stage) {
    case Stage::kQueue:
      return "queue";
    case Stage::kResolve:
      return "resolve";
    case Stage::kConnect:
      return "connect";
    case Stage::kProxy:
      return "proxy";
    case Stage::kTls:
      return "tls";
    case Stage::kSend:
      return "send";
    case Stage::kResponse:
      return "response";
    case Stage::kBody:
      return "body";
  }
  return "unknown";
}

Error AttemptError::to_error() const {
  if (stage == Stage::kQueue) {
    return Error{.code = ErrorCode::kOverloaded,
                 .message = "到上游的连接已满，请稍后重试",
                 .retry_after = {}};
  }
  if (maybe_sent) {
    // 请求可能已经被上游处理并计费：504，不建议客户端自动重试（design.md 6.1）
    return Error{.code = ErrorCode::kUpstreamTimeout,
                 .message = timed_out ? "上游响应超时" : "上游在响应之前断开了连接",
                 .retry_after = {}};
  }
  return Error{.code = ErrorCode::kUpstreamUnavailable,
               .message = timed_out ? "连接上游超时" : "无法连接上游",
               .retry_after = {}};
}

std::size_t per_worker_limit(std::uint32_t total, unsigned worker_count) {
  const std::size_t workers = std::max(1U, worker_count);
  return std::max<std::size_t>(1, (static_cast<std::size_t>(total) + workers - 1) / workers);
}

namespace {

auto failure(Stage stage, bool maybe_sent, std::string detail) {
  return unexpected(
      AttemptError{.stage = stage, .maybe_sent = maybe_sent, .detail = std::move(detail)});
}

// ---- 两个阶段 ----

// 连接阶段：拿到一个带连接的 Lease。progress 是当前阶段（超时时用）
Task<Outcome<Lease>> connect_phase(std::shared_ptr<Pool> pool, Route route, detail::DnsCache* dns,
                                   Stage* progress) {
  *progress = Stage::kQueue;
  std::optional<Lease> lease = co_await detail::acquire(std::move(pool));
  if (!lease) co_return failure(Stage::kQueue, false, "排队时被取消");
  if (lease->conn() != nullptr) co_return std::move(*lease);  // 复用
  auto conn = co_await detail::establish(route, dns, progress);
  if (!conn) co_return unexpected(conn.error());  // lease 析构：归还名额
  lease->set_connection(std::move(*conn));
  co_return std::move(*lease);
}

struct Exchanged {
  std::unique_ptr<detail::Parser> parser;  ///< 已经读完最终响应的响应头
};

// 交换阶段：写请求、读响应头。复用的连接写失败时换新连接重发一次（上游一定没有收到完整的请求）
Task<Outcome<Exchanged>> exchange_phase(Lease* lease, const detail::Message* msg, Route route,
                                        detail::DnsCache* dns, Stage* progress) {
  for (bool retried = false;; retried = true) {
    Connection* conn = lease->conn();
    const bool reused = conn->requests > 0;
    *progress = Stage::kSend;
    ++conn->requests;
    auto [wec, wn] =
        co_await conn->with_stream([&](auto& s) { return http::async_write(s, *msg, kTuple); });
    if (wec) {
      if (!reused || retried)
        co_return failure(Stage::kSend, false, "写请求失败：" + wec.message());
      lease->drop_connection();  // 多半是上游刚关闭了这条空闲连接
      auto fresh = co_await detail::establish(route, dns, progress);
      if (!fresh) co_return unexpected(fresh.error());
      lease->set_connection(std::move(*fresh));
      continue;
    }
    *progress = Stage::kResponse;
    auto head = co_await detail::read_final_head(conn, msg->method() == http::verb::head);
    if (!head) {
      AttemptError e = head.error();
      e.reused_without_response = reused && e.reused_without_response;
      co_return unexpected(std::move(e));
    }
    co_return Exchanged{std::move(*head)};
  }
}

}  // namespace

// ---- Client ----

struct Client::State {
  explicit State(ClientOptions o)
      : opt(std::move(o)),
        dns(opt.executor, opt.resolver, opt.dns_ttl),
        sweep_timer(opt.executor) {}
  ClientOptions opt;
  detail::DnsCache dns;
  std::map<std::string, std::shared_ptr<Pool>, std::less<>> pools;
  asio::steady_timer sweep_timer;

  std::shared_ptr<Pool> pool_for(const config::Provider& p) {
    auto it = pools.find(p.name);
    if (it == pools.end()) {
      it = pools
               .emplace(p.name, std::make_shared<Pool>(
                                    p.name, per_worker_limit(p.max_connections, opt.worker_count),
                                    opt.metrics))
               .first;
    }
    return it->second;
  }
  void count(const std::string& provider, std::string_view kind) const {
    if (opt.metrics != nullptr)
      opt.metrics->increment(Counter::kUpstreamConnections, {provider, kind});
  }
};

Client::Client(ClientOptions options) : state_(std::make_shared<State>(std::move(options))) {
  asio::co_spawn(state_->opt.executor, sweep_loop(state_), asio::detached);
}

Client::~Client() {
  try {
    state_->sweep_timer.cancel();
    for (auto& [name, pool] : state_->pools) pool->close_idle();
  } catch (...) {
    // 析构中不能抛出：空闲连接随 State 一起释放
    std::fputs("[upstream] closing idle connections failed\n", stderr);
  }
}

Client::PoolStats Client::stats(const std::string& provider) const {
  auto it = state_->pools.find(provider);
  if (it == state_->pools.end()) return {};
  return {.idle = it->second->idle_count(),
          .busy = it->second->busy_count(),
          .waiting = it->second->waiting_count()};
}

Task<> Client::sweep_loop(std::weak_ptr<State> weak) {
  for (;;) {
    auto st = weak.lock();
    if (!st) co_return;
    st->sweep_timer.expires_after(st->opt.sweep_interval);
    auto* timer = &st->sweep_timer;
    st.reset();  // 等待期间不延长 State 的生命周期
    auto [ec] = co_await timer->async_wait(kTuple);
    st = weak.lock();
    if (!st || ec) co_return;  // Client 已经析构（析构时取消了定时器）
    for (auto& [name, pool] : st->pools)
      pool->sweep(SteadyClock::now(), st->opt.config->upstream.idle_timeout);
  }
}

Task<Outcome<Response>> Client::send(const config::Provider* provider_ptr, Request request,
                                     Deadlines deadlines) {
  const std::shared_ptr<State> st = state_;  // send 期间 Client 被析构时，State 仍然有效
  const config::Provider& provider = *provider_ptr;
  Route route{.provider = &provider, .proxy = st->opt.config->proxy_for(provider), .tls = nullptr};
  if (provider.base_url.scheme == "https") {
    route.tls = st->opt.tls ? st->opt.tls->find(provider.ca_file) : nullptr;
    if (route.tls == nullptr)
      co_return failure(Stage::kTls, false, "没有为这个上游创建 TLS 上下文");
  }
  Outcome<detail::Message> built = failure(Stage::kSend, false, "构造请求失败");
  try {
    built = detail::build_message(route, std::move(request));
  } catch (const std::exception& e) {  // 比如内存不足：请求还没有开始发送
    co_return failure(Stage::kSend, false, std::string("构造请求失败：") + e.what());
  }
  if (!built) co_return unexpected(built.error());
  try {
    const detail::Message& msg = *built;
    asio::steady_timer timer(st->opt.executor);

    // 1. 连接阶段
    Stage progress = Stage::kQueue;
    timer.expires_after(deadlines.connect);
    auto c = co_await (connect_phase(st->pool_for(provider), route, &st->dns, &progress) ||
                       timer.async_wait(kTuple));
    if (c.index() == 1) {
      if (progress == Stage::kQueue) st->count(provider.name, "queue_timeout");
      co_return unexpected(AttemptError{
          .stage = progress,
          .maybe_sent = false,
          .timed_out = true,
          .reused_without_response = false,
          .detail = std::string("连接阶段超时（") + std::string(stage_name(progress)) + "）"});
    }
    auto& connected = std::get<0>(c);
    if (!connected) co_return unexpected(connected.error());
    Lease lease = std::move(*connected);
    st->count(provider.name, lease.conn()->requests > 0 ? "reused" : "new");

    // 2. 交换阶段
    progress = Stage::kSend;
    timer.expires_after(deadlines.first_byte);
    auto x = co_await (exchange_phase(&lease, &msg, route, &st->dns, &progress) ||
                       timer.async_wait(kTuple));
    if (x.index() == 1) {
      // 超时时还在写请求：请求不完整，上游不可能处理（maybe_sent = false）
      const bool sent = progress == Stage::kResponse;
      co_return unexpected(AttemptError{
          .stage = progress,
          .maybe_sent = sent,
          .timed_out = true,
          .reused_without_response = false,
          .detail =
              sent ? "等待响应头超时"
                   : std::string("发送阶段超时（") + std::string(stage_name(progress)) + "）"});
    }
    auto& exchanged = std::get<0>(x);
    if (!exchanged) co_return unexpected(exchanged.error());

    Response response;
    response.status = static_cast<int>(exchanged->parser->get().result_int());
    if (route.absolute_form() && response.status == 407 &&
        exchanged->parser->get().count(http::field::proxy_authenticate) > 0) {
      // 绝对路径形式下，带 Proxy-Authenticate 的 407 是代理的拒绝，请求没有被转发。
      // 不带的 407 是上游自己返回的：上游已经处理了请求，按普通响应返回（独立审查 R11）
      co_return failure(Stage::kProxy, false,
                        "代理拒绝了请求，状态码 407（代理要求认证，或者用户名密码不对）");
    }
    for (const auto& f : exchanged->parser->get()) {
      response.headers.push_back(Header{std::string(f.name_string()), std::string(f.value())});
    }
    response.body = detail::make_body(std::move(lease), std::move(exchanged->parser));
    co_return response;
  } catch (const std::exception& e) {
    // 不知道失败在哪一步：按“可能已发送”处理，宁可不重试，也不冒重复扣费的风险
    co_return failure(Stage::kResponse, true, std::string("内部错误：") + e.what());
  }
}

}  // namespace llmgw::upstream
