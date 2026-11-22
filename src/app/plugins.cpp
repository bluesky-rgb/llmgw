#include "plugins.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <utility>

#include "llmgw/middleware/auth.h"

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
  if (!report.snapshot->keys.empty() && !has_auth(*report.snapshot)) {
    report.issues.push_back({.severity = config::Severity::kWarning,
                             .path = "keys",
                             .message =
                                 "配置了网关 key，但 pipeline 中没有 auth 插件：不会校验 key，"
                                 "任何请求都会被放行。启用：\"pipeline\": [\"auth\"]"});
  }
  std::ranges::move(issues, std::back_inserter(report.issues));
  if (has_error) report.snapshot.reset();
  return report;
}

bool has_auth(const config::Snapshot& snapshot) {
  return std::ranges::any_of(snapshot.pipeline,
                             [](const config::PipelineEntry& e) { return e.name == "auth"; });
}

int keygen(const std::vector<std::string_view>& args) {
  std::string id;
  for (std::size_t i = 0; i < args.size(); ++i) {
    if (args[i] == "--id" && i + 1 < args.size()) {
      id = std::string(args[++i]);
    } else {
      std::fprintf(stderr, "未知参数：%s\n用法：llmgw keygen --id <名字>\n",
                   std::string(args[i]).c_str());
      return 2;
    }
  }
  const bool id_ok = !id.empty() && id.size() <= 64 && std::ranges::all_of(id, [](char c) {
    return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '-' || c == '_' || c == '.';
  });
  if (!id_ok) {
    std::fprintf(stderr,
                 "用法：llmgw keygen --id <名字>\n名字只能包含字母、数字和 \"-_.\"，长度 1–64，"
                 "比如 app-chatbot（日志和用量中用它标识 key）\n");
    return 2;
  }
  const std::string key = middleware::generate_key();
  if (key.empty()) {
    std::fprintf(stderr, "生成随机数失败（OpenSSL RAND_bytes）\n");
    return 1;
  }
  // key 只输出到标准输出（方便重定向到文件或密钥管理工具）；说明文字输出到标准错误
  std::fprintf(stderr, "网关 key（只显示这一次，网关不保存它，请交给使用方并妥善保管）：\n");
  std::printf("%s\n", key.c_str());
  std::fflush(stdout);
  std::fprintf(stderr,
               "\n把下面这一项加入配置的 keys（并确认 pipeline 中有 \"auth\"）：\n"
               "  { \"id\": \"%s\", \"hash\": \"%s\" }\n",
               id.c_str(), middleware::hash_key(key).c_str());
  return 0;
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
