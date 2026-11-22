// 中间件链（任务 2.1）：每个 Worker 一条，按 pipeline 的顺序执行 on_request，按相反的顺序执行
// on_complete。只被 Session 调用（Session 是唯一的编排者，design.md 3.2）。
#pragma once

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "llmgw/config/snapshot.h"
#include "llmgw/core/request_context.h"
#include "llmgw/core/response.h"
#include "llmgw/core/result.h"
#include "llmgw/core/task.h"
#include "llmgw/middleware/middleware.h"
#include "llmgw/middleware/registry.h"

namespace llmgw::middleware {

class Chain {
 public:
  /// 一个请求在链上的进度。由调用者（Session）放在自己的栈帧中，生命周期覆盖 on_request 到
  /// on_complete。
  struct Run {
    std::size_t entered = 0;  ///< on_request 被调用过的插件个数（前 entered 个）
    std::size_t completed = 0;  ///< 已经执行完 on_complete 的个数（从第 entered-1 个往前数）
    /// on_complete 被取消（Session
    /// 的限时到了）时正在执行的插件。插件不理会取消、超时之后照样返回时， 记的也是它，而不是下一个
    std::optional<std::size_t> cut_off;
    /// on_request 的拒绝不是插件给出的，而是因为某个插件抛了异常（500，终态是 kFailed 而不是
    /// kRejected）
    bool failed = false;
    std::vector<Header> response_headers;  ///< 各插件附加的响应头，按插件的顺序
  };

  /// 按 snapshot.pipeline 创建各插件的实例。snapshot 必须已经通过 validate_pipeline。
  /// 某个插件的 create 返回 nullptr 时返回错误（说明是哪个插件）。
  [[nodiscard]] static Result<std::unique_ptr<Chain>> build(const config::Snapshot& snapshot,
                                                            const Registry& registry,
                                                            const WorkerResources& worker);
  /// 测试用：直接用给定的实例组成链（names 与 plugins 一一对应，用于日志和指标）。
  Chain(std::vector<std::string> names, std::vector<std::unique_ptr<Middleware>> plugins,
        MetricsRecorder* metrics);

  [[nodiscard]] bool empty() const { return plugins_.empty(); }
  [[nodiscard]] std::size_t size() const { return plugins_.size(); }
  /// 第 i 个插件的名字（日志和指标用）
  [[nodiscard]] const std::string& name(std::size_t i) const { return names_.at(i); }

  /// 依次执行 on_request，遇到拒绝就停下，返回拒绝的结果（否则放行）。各插件附加的响应头累积在
  /// run->response_headers 中。插件抛异常时返回 500（kInternal），抛异常的插件也算“被调用过”。
  /// 被取消（客户端断开、总超时）时不再进入下一个插件。ctx、run 在调用期间必须有效。
  [[nodiscard]] Task<Decision> on_request(RequestContext* ctx, Run* run);

  /// 按相反的顺序，对前 run->entered 个插件执行
  /// on_complete。某个插件抛异常时记录日志和指标，继续执行 其余的。被取消（Session 的 5
  /// 秒限时）时停下。ctx、run 在调用期间必须有效。
  [[nodiscard]] Task<> on_complete(const RequestContext* ctx, Run* run);

 private:
  std::vector<std::string> names_;
  std::vector<std::unique_ptr<Middleware>> plugins_;
  std::unique_ptr<MetricsRecorder> null_metrics_;  // 没有传入 metrics 时使用
  MetricsRecorder* metrics_;
};

}  // namespace llmgw::middleware
