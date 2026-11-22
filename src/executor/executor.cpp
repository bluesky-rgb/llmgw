// Executor 的实现。流程见 docs/tasks/1.7-router-executor.md 第 2、4、5 节。
#include "llmgw/executor/executor.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "classify.h"
#include "llmgw/provider/provider.h"

namespace llmgw::executor {
namespace {

using detail::Next;
using Returned = Result<std::unique_ptr<ResponseStream>>;

// 在上游的响应头之外加上 X-LLMGW-Provider、X-LLMGW-Upstream-Model、X-LLMGW-Attempts
// （design.md 第 5 节的客户端契约）。其余原样转发
class AnnotatedStream final : public ResponseStream {
 public:
  AnnotatedStream(std::unique_ptr<ResponseStream> inner, const config::TargetConfig& target,
                  std::size_t attempts)
      : inner_(std::move(inner)), head_(inner_->head()) {
    head_.headers.push_back({"X-LLMGW-Provider", target.provider});
    head_.headers.push_back({"X-LLMGW-Upstream-Model", target.model});
    head_.headers.push_back({"X-LLMGW-Attempts", std::to_string(attempts)});
  }
  [[nodiscard]] const ResponseHead& head() const override { return head_; }
  Task<Result<std::optional<Chunk>>> next() override { return inner_->next(); }

 private:
  std::unique_ptr<ResponseStream> inner_;
  ResponseHead head_;
};

// 错误信息中的一次尝试：只有失败的分类。上游名、AttemptError::detail（主机名、IP）都不放进
// 返回给客户端的错误（design.md 6.10）；上游名可以从 ctx->attempts 中查到
std::string describe(AttemptResult result, int status) {
  switch (result) {
    case AttemptResult::kConnectError:
      return "连接失败";
    case AttemptResult::kTimeout:
      return "超时";
    case AttemptResult::kBreakerOpen:
      return "熔断";
    default:
      return status > 0 ? std::to_string(status) : std::string(label(result));
  }
}

// 上游建议的重试间隔：Retry-After（秒）或 retry-after-ms（向上取整到秒）。HTTP 日期格式不解析
std::optional<std::chrono::seconds> retry_after(const ResponseHead& head) {
  auto number = [](std::string_view v) -> std::optional<std::int64_t> {
    std::int64_t n = 0;
    const auto [end, ec] = std::from_chars(v.data(), v.data() + v.size(), n);
    if (ec != std::errc() || end != v.data() + v.size() || n < 0) return std::nullopt;
    return n;
  };
  auto iequals = [](std::string_view a, std::string_view b) {
    return std::ranges::equal(a, b, [](char x, char y) {
      return std::tolower(static_cast<unsigned char>(x)) ==
             std::tolower(static_cast<unsigned char>(y));
    });
  };
  constexpr std::int64_t kMaxSeconds = std::int64_t{24} * 3600;  // 不合理的值不转告客户端
  for (const auto& h : head.headers) {
    if (iequals(h.name, "retry-after")) {
      if (auto n = number(h.value); n && *n <= kMaxSeconds) return std::chrono::seconds(*n);
    } else if (iequals(h.name, "retry-after-ms")) {
      if (auto n = number(h.value); n && *n <= kMaxSeconds * 1000)
        return std::chrono::seconds((*n + 999) / 1000);
    }
  }
  return std::nullopt;
}

// 一次 execute() 中跨尝试的记录
struct Tally {
  std::size_t attempts = 0;
  bool ambiguous = false;  ///< 有尝试可能已经被上游处理（只有 retry_on_ambiguous 时才会继续）
  bool out_of_time = false;
  std::vector<std::string> tried;  ///< 每次失败的分类，错误信息用
  /// 最后一次尝试是 429 / 502 / 503 时：它的状态码和 Retry-After（第 4 节规则 2）
  std::optional<std::pair<int, std::optional<std::chrono::seconds>>> last_busy;
};

std::string join(const std::vector<std::string>& parts) {
  std::string out;
  for (const auto& p : parts) {
    if (!out.empty()) out += "，";
    out += p;
  }
  return out;
}

// 所有目标都没有可以返回的结果（第 4 节，按顺序判断）
Error exhausted(const Tally& t) {
  const std::string tried =
      t.tried.empty() ? std::string("没有尝试任何上游")
                      : "尝试了 " + std::to_string(t.tried.size()) + " 次：" + join(t.tried);
  if (t.ambiguous) {  // 1. 可能已经计费：504，不要重试（即使最后一次是 429 也不让客户端重来）
    return Error{.code = ErrorCode::kUpstreamTimeout,
                 .message = "所有上游都失败了（" + tried +
                            "）。其中有的上游可能已经处理了这个请求，请不要自动重试",
                 .retry_after = {}};
  }
  if (t.last_busy) {  // 2. 最后一次是 429 / 502 / 503：保留状态码的含义和 Retry-After，不转发响应体
    const auto [status, after] = *t.last_busy;
    return Error{.code = status == 429 ? ErrorCode::kRateLimited : ErrorCode::kUpstreamUnavailable,
                 .message = std::string(status == 429 ? "所有上游都在限流" : "所有上游都不可用") +
                            "（" + tried + "）",
                 .retry_after = after};
  }
  if (t.out_of_time) {
    return Error{.code = ErrorCode::kUpstreamUnavailable,
                 .message = "请求的总时间已经用完，所有上游都没有成功（" + tried + "）",
                 .retry_after = {}};
  }
  return Error{.code = ErrorCode::kUpstreamUnavailable,
               .message = "所有上游都失败了（" + tried + "）",
               .retry_after = {}};
}

void record(const Options& options, RequestContext* ctx, const config::TargetConfig& t,
            AttemptResult result, int status, SteadyClock::duration elapsed) {
  ctx->attempts.push_back({.target = {.provider = t.provider, .model = t.model},
                           .result = result,
                           .upstream_status = status,
                           .elapsed = elapsed});
  if (options.metrics == nullptr) return;
  options.metrics->increment(Counter::kUpstreamAttempts, {t.provider, label(result)});
  if (result == AttemptResult::kCancelled) return;  // 不是上游的问题：不计耗时、不计失败
  options.metrics->observe(Histogram::kUpstreamDuration, {t.provider},
                           std::chrono::duration<double>(elapsed).count());
  if (detail::is_auth_error(status)) {
    options.metrics->increment(Counter::kUpstreamAuthErrors, {t.provider});
  }
}

// 尝试一个目标。有值：把它交给客户端（响应或错误）；没有值：换下一个（原因记在 tally 中）。
// 协程的参数都用指针（code-architecture.md 3.4）
Task<std::optional<Returned>> try_target(const Options* options, RequestContext* ctx,
                                         const config::TargetConfig* target, bool last,
                                         Tally* tally) {
  const config::TargetConfig& t = *target;
  const config::Snapshot& snapshot = *ctx->config;
  const auto start = SteadyClock::now();
  const auto remaining = ctx->request.deadline - start;
  if (remaining <= SteadyClock::duration::zero()) {  // 不再开始新的尝试
    tally->out_of_time = true;
    co_return std::nullopt;
  }
  const config::Provider* p = snapshot.find_provider(t.provider);
  if (p == nullptr) {  // 配置加载时已经校验过引用，不会发生；防御：当作连接失败
    ++tally->attempts;
    record(*options, ctx, t, AttemptResult::kConnectError, 0, {});
    tally->tried.push_back(describe(AttemptResult::kConnectError, 0));
    tally->last_busy.reset();
    co_return std::nullopt;
  }
  // 每个目标的改写规则不同：每次都从原始请求体重新改写。失败是客户端的错误（400），
  // 换哪个目标都一样，直接返回
  auto prepared = provider::prepare(ctx->request, *p, t);
  if (!prepared) co_return Returned(fail(prepared.error()));

  ctx->usage = {};  // 上一次尝试的预读可能已经写入了用量、首字延迟
  ctx->ttft.reset();
  // 每个阶段的时限按目标的配置，并且不超过剩余的总时间。各阶段依次进行，加起来仍可能超过
  // 剩余时间：总时间由 Session 的竞速保证，这里只是不让单个定时器设得比请求还长
  const auto& to = t.timeouts;
  const SteadyClock::duration first_byte = ctx->request.stream
                                               ? SteadyClock::duration(to.first_byte)
                                               : SteadyClock::duration(to.first_byte_non_stream);
  const upstream::Deadlines deadlines{
      .connect = std::min(SteadyClock::duration(to.connect), remaining),
      .first_byte = std::min(first_byte, remaining)};
  const SteadyClock::duration prefetch_idle = std::min(SteadyClock::duration(to.idle), remaining);
  ++tally->attempts;
  auto out = co_await provider::attempt(options->client, p, std::move(*prepared), deadlines,
                                        prefetch_idle, ctx);
  const auto elapsed = SteadyClock::now() - start;
  // 被取消（客户端断开、总超时）：不是上游的问题，不计入失败，也不再尝试下一个。
  // task.h 的规则：先检查再产生副作用
  if (is_cancelled(co_await kCancellationState)) {
    record(*options, ctx, t, AttemptResult::kCancelled, 0, elapsed);
    co_return Returned(fail(ErrorCode::kInternal, "请求已被取消"));
  }
  // 最后一个目标没有下一个可换：按默认（不重试）处理“不确定”的结果
  const bool retry_ambiguous = snapshot.retry.retry_on_ambiguous && !last;
  const int status = out ? (*out)->head().status : 0;
  const detail::Verdict v = out ? detail::classify(status, retry_ambiguous)
                                : detail::classify(out.error(), retry_ambiguous);
  record(*options, ctx, t, v.result, status, elapsed);
  tally->ambiguous = tally->ambiguous || v.ambiguous;
  if (v.next == Next::kReturn) {
    if (!out) co_return Returned(fail(out.error().to_error()));  // 可能已经计费：504
    ctx->upstream_idle = t.timeouts.idle;
    co_return Returned(std::unique_ptr<ResponseStream>(
        std::make_unique<AnnotatedStream>(std::move(*out), t, tally->attempts)));
  }
  tally->tried.push_back(describe(v.result, status));
  if (out && detail::forward_when_exhausted(status)) {
    tally->last_busy.emplace(status, retry_after((*out)->head()));
  } else {
    tally->last_busy.reset();
  }
  co_return std::nullopt;  // out 在这里销毁：响应体没有读完，连接关闭，不还回池中
}

}  // namespace

Executor::Executor(Options options) : options_(options) {}

Task<Result<std::unique_ptr<ResponseStream>>> Executor::execute(
    RequestContext* ctx, std::span<const config::TargetConfig* const> targets) {
  if (targets.empty()) co_return fail(ErrorCode::kUpstreamUnavailable, "这个模型没有配置目标");
  Tally tally;
  for (std::size_t i = 0; i < targets.size(); ++i) {
    // 两次尝试之间也检查（防御：单线程下这里之前没有挂起点，正常情况下不会在这里发现取消）
    if (is_cancelled(co_await kCancellationState)) {
      co_return fail(ErrorCode::kInternal, "请求已被取消");
    }
    auto r = co_await try_target(&options_, ctx, targets[i], i + 1 == targets.size(), &tally);
    if (r) co_return std::move(*r);
    if (tally.out_of_time) break;
  }
  co_return fail(exhausted(tally));
}

}  // namespace llmgw::executor
