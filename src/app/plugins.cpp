#include "plugins.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <utility>

namespace llmgw::app {

std::shared_ptr<const middleware::Registry> builtin_registry() {
  auto registry = std::make_shared<middleware::Registry>();
  if (auto r = middleware::register_builtin(registry.get()); !r) {
    // 程序本身的错误（内置插件的名字不合法或重名），单元测试 Registry.builtin_registration_succeeds
    // 会发现
    std::fprintf(stderr, "内置插件登记失败：%s\n", r.error().message.c_str());
    std::abort();
  }
  return registry;
}

config::LoadReport load_config(const std::string& path, const config::LoadOptions& options,
                               const middleware::Registry& registry) {
  config::LoadReport report = config::load_file(path, options);
  if (!report.ok()) return report;  // 有错误时快照为空，pipeline 可能不完整：不再校验插件
  auto issues = middleware::validate_pipeline(*report.snapshot, registry);
  const bool has_error = std::ranges::any_of(
      issues, [](const config::Issue& i) { return i.severity == config::Severity::kError; });
  std::ranges::move(issues, std::back_inserter(report.issues));
  if (has_error) report.snapshot.reset();
  return report;
}

server::ChainFactory make_chain_factory(std::shared_ptr<const config::Snapshot> snapshot,
                                        std::shared_ptr<const middleware::Registry> registry) {
  if (snapshot->pipeline.empty()) return {};
  return [snapshot = std::move(snapshot), registry = std::move(registry)](
             const server::WorkerInfo& info,
             MetricsRecorder* metrics) -> Result<std::unique_ptr<middleware::Chain>> {
    return middleware::Chain::build(*snapshot, *registry,
                                    middleware::WorkerResources{.index = info.index,
                                                                .worker_count = info.worker_count,
                                                                .executor = info.executor,
                                                                .metrics = metrics});
  };
}

int list_plugins(const middleware::Registry& registry) {
  const auto all = registry.all();
  if (all.empty()) {
    std::printf("这个版本没有内置的中间件插件（鉴权、限流、预算在 v0.3 提供）\n");
    return 0;
  }
  std::printf("中间件插件（在配置的 pipeline 中按名字启用）：\n");
  for (const auto* f : all) {
    std::printf("  %-12.*s %.*s\n", static_cast<int>(f->name().size()), f->name().data(),
                static_cast<int>(f->description().size()), f->description().data());
  }
  return 0;
}

}  // namespace llmgw::app
