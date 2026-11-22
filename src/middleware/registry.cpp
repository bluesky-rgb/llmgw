// 插件注册表与 pipeline 的校验（任务 2.1）。只在启动时执行。
#include "llmgw/middleware/registry.h"

#include <algorithm>
#include <cstddef>
#include <string>
#include <utility>

namespace llmgw::middleware {
namespace {

bool valid_name(std::string_view name) {
  return !name.empty() && std::ranges::all_of(name, [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
  });
}

// 编辑距离（插件名很短，O(n·m) 足够）。只在配置有错误时调用
std::size_t edit_distance(std::string_view a, std::string_view b) {
  std::vector<std::size_t> prev(b.size() + 1);
  std::vector<std::size_t> cur(b.size() + 1);
  for (std::size_t j = 0; j <= b.size(); ++j) prev[j] = j;
  for (std::size_t i = 1; i <= a.size(); ++i) {
    cur[0] = i;
    for (std::size_t j = 1; j <= b.size(); ++j) {
      cur[j] =
          std::min({prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + (a[i - 1] == b[j - 1] ? 0 : 1)});
    }
    std::swap(prev, cur);
  }
  return prev[b.size()];
}

std::string unknown_plugin_message(std::string_view name, const Registry& registry) {
  std::string msg = "没有名为 \"" + std::string(name) + "\" 的插件";
  const MiddlewareFactory* best = nullptr;
  std::size_t best_distance = 3;  // 编辑距离不超过 2 才给建议
  std::string available;
  for (const MiddlewareFactory* f : registry.all()) {
    const std::size_t d = edit_distance(name, f->name());
    if (d < best_distance) {
      best_distance = d;
      best = f;
    }
    if (!available.empty()) available += "、";
    available += f->name();
  }
  if (best != nullptr) msg += "（是不是 \"" + std::string(best->name()) + "\"？）";
  msg += available.empty() ? "；这个版本没有内置的插件（鉴权、限流、预算在 v0.3 提供）"
                           : "；可用的插件：" + available + "（llmgw plugins 列出说明）";
  return msg;
}

}  // namespace

Result<void> Registry::add(std::unique_ptr<MiddlewareFactory> factory) {
  if (factory == nullptr) return fail(ErrorCode::kInternal, "插件工厂为空");
  const std::string_view name = factory->name();
  if (!valid_name(name)) {
    return fail(ErrorCode::kInternal,
                "插件名 \"" + std::string(name) + "\" 不合法：只能包含小写字母、数字和下划线");
  }
  if (find(name) != nullptr) {
    return fail(ErrorCode::kInternal, "插件 \"" + std::string(name) + "\" 重复登记");
  }
  const auto pos =
      std::ranges::lower_bound(factories_, name, {}, [](const auto& f) { return f->name(); });
  factories_.insert(pos, std::move(factory));
  return {};
}

const MiddlewareFactory* Registry::find(std::string_view name) const {
  const auto it =
      std::ranges::lower_bound(factories_, name, {}, [](const auto& f) { return f->name(); });
  return it != factories_.end() && (*it)->name() == name ? it->get() : nullptr;
}

std::vector<const MiddlewareFactory*> Registry::all() const {
  std::vector<const MiddlewareFactory*> out;
  out.reserve(factories_.size());
  for (const auto& f : factories_) out.push_back(f.get());
  return out;
}

Result<void> register_builtin(Registry* registry) {
  // 内置的插件在任务 2.2–2.4 中加入，每个一行：
  //   if (auto r = registry->add(std::make_unique<AuthFactory>()); !r) return r;
  (void)registry;
  return {};
}

std::vector<config::Issue> validate_pipeline(const config::Snapshot& snapshot,
                                             const Registry& registry) {
  std::vector<config::Issue> issues;
  for (const config::PipelineEntry& entry : snapshot.pipeline) {
    const MiddlewareFactory* factory = registry.find(entry.name);
    if (factory == nullptr) {
      issues.push_back({.severity = config::Severity::kError,
                        .path = entry.path,
                        .message = unknown_plugin_message(entry.name, registry)});
      continue;
    }
    auto found = factory->validate(
        PluginSpec{.name = entry.name, .config_json = entry.config_json, .path = entry.path},
        snapshot);
    std::ranges::move(found, std::back_inserter(issues));
  }
  return issues;
}

}  // namespace llmgw::middleware
