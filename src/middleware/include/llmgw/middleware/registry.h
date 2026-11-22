// 插件注册表（任务 2.1）：启动时登记所有插件，之后只读，所有 Worker 共享。
//
// 显式登记，不用“静态初始化时自动登记”的宏（ADR-009）：插件放在静态库里时，没有被引用的目标文件
// 会被链接器丢掉，宏就悄悄失效；还会引入静态初始化顺序问题和全局可变状态。
#pragma once

#include <memory>
#include <string_view>
#include <vector>

#include "llmgw/config/loader.h"
#include "llmgw/config/snapshot.h"
#include "llmgw/core/result.h"
#include "llmgw/middleware/middleware.h"

namespace llmgw::middleware {

class Registry {
 public:
  /// 登记一个插件。名字为空、含有小写字母、数字、下划线以外的字符，或者与已有的插件重名时返回错误。
  [[nodiscard]] Result<void> add(std::unique_ptr<MiddlewareFactory> factory);
  /// 找不到返回 nullptr。
  [[nodiscard]] const MiddlewareFactory* find(std::string_view name) const;
  /// 所有插件，按名字排序。
  [[nodiscard]] std::vector<const MiddlewareFactory*> all() const;

 private:
  std::vector<std::unique_ptr<MiddlewareFactory>> factories_;  // 按名字排序
};

/// 登记内置的插件（auth、ratelimit、budget 在任务 2.2–2.4 中加入）。任何一个登记失败（名字不合法、
/// 重名）都返回错误，不能让插件悄悄“消失”（ADR-009 要避免的正是这种情况）。
[[nodiscard]] Result<void> register_builtin(Registry* registry);

/// 校验 snapshot.pipeline：每个插件名都已登记（拼错时给出建议，并列出可用的插件），再由各插件的
/// validate 校验自己的配置。重复的插件名已经由 config 拒绝。
[[nodiscard]] std::vector<config::Issue> validate_pipeline(const config::Snapshot& snapshot,
                                                           const Registry& registry);

}  // namespace llmgw::middleware
