// Executor：按目标列表依次尝试，按重试规则表决定返回还是换下一个（任务 1.7，design.md 6.1）。
//
// 核心原则：请求可能已经被上游计费时不重试（除非配置了 retry.retry_on_ambiguous）。
// 熔断、重试预算、舱壁隔离在 2.5 中加入（同样放在这里：每个 Worker 一份的状态）。
#pragma once

#include <memory>
#include <span>

#include "llmgw/config/snapshot.h"
#include "llmgw/core/metrics.h"
#include "llmgw/core/request_context.h"
#include "llmgw/core/response.h"
#include "llmgw/core/result.h"
#include "llmgw/core/task.h"
#include "llmgw/upstream/client.h"

namespace llmgw::executor {

struct Options {
  upstream::Client* client = nullptr;  ///< 这个 Worker 的；必须比 Executor 活得久
  MetricsRecorder* metrics = nullptr;  ///< 可以为空
};

/// 每个 Worker 一个实例，只在该 Worker 的线程上使用。
class Executor {
 public:
  explicit Executor(Options options);

  /// 依次尝试 targets（指向 ctx->config 中的快照；调用期间必须有效）。
  /// - 成功：ResponseStream——2xx，或者原样转发的上游响应（其他 4xx；500、504 等不确定的结果）。
  ///   响应头中加上 X-LLMGW-Provider、X-LLMGW-Upstream-Model、X-LLMGW-Attempts
  /// - 失败：网关自己的错误（400 请求体不合法；429 / 502 所有上游都失败，带上游的 Retry-After；
  ///   504 可能已经计费）。错误信息只有失败的分类，不包含上游名和地址
  ///
  /// 每次尝试追加一条 ctx->attempts；每次尝试之前重置 ctx->usage、ctx->ttft；返回响应之前写入
  /// ctx->upstream_idle。被取消时（客户端断开、总超时）尽快返回一个错误，调用方应先检查取消状态。
  /// 不抛异常。
  [[nodiscard]] Task<Result<std::unique_ptr<ResponseStream>>> execute(
      RequestContext* ctx, std::span<const config::TargetConfig* const> targets);

 private:
  Options options_;
};

}  // namespace llmgw::executor
