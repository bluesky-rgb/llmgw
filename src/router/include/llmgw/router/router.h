// 路由：对外的模型名 → 按尝试顺序排列的目标（任务 1.7）。
//
// RoutingStrategy 是扩展点（extensibility.md 第 3 节）；v1.0 只有内置的顺序切换。
// 路由只决定“按什么顺序试”，“试了失败怎么办”由 executor 决定（重试规则是核心，不是扩展点）。
#pragma once

#include <memory>
#include <vector>

#include "llmgw/config/snapshot.h"
#include "llmgw/core/request_context.h"
#include "llmgw/core/result.h"

namespace llmgw::router {

/// 一次路由的结果。指针指向 ctx->config 中的快照，与请求同生命周期。
struct Route {
  const config::ModelConfig* model = nullptr;
  std::vector<const config::TargetConfig*> targets;  ///< 尝试的顺序；不为空
};

/// 路由策略。每个 Worker 一个实例，只在该 Worker 的线程上调用：有状态的策略（轮询、按延迟）
/// 不需要加锁。同步接口：路由是纯计算，不做 I/O。
class RoutingStrategy {
 public:
  RoutingStrategy() = default;
  RoutingStrategy(const RoutingStrategy&) = delete;
  RoutingStrategy& operator=(const RoutingStrategy&) = delete;
  RoutingStrategy(RoutingStrategy&&) = delete;
  RoutingStrategy& operator=(RoutingStrategy&&) = delete;
  virtual ~RoutingStrategy() = default;

  /// 模型不存在 → 404 model_not_found。ctx->config 不能为空。
  [[nodiscard]] virtual Result<Route> route(const RequestContext& ctx) = 0;
};

/// 内置策略：按配置中的顺序依次尝试（failover）。
[[nodiscard]] std::unique_ptr<RoutingStrategy> make_failover();

}  // namespace llmgw::router
