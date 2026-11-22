// llmgw 命令行入口：check（任务 1.1）、serve（任务 1.7）、plugins（任务 2.1）；status、reload
// 等在 3.4 中加入。
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

#include "llmgw/config/loader.h"
#include "llmgw/core/version.h"
#include "plugins.h"
#include "serve.h"

namespace {

constexpr int kOk = 0;
constexpr int kConfigError = 1;
constexpr int kUsageError = 2;

void print_usage() {
  std::fputs(
      "用法：\n"
      "  llmgw serve [-c 配置文件] [--allow-unauthenticated]\n"
      "                                           启动网关（Ctrl-C 或 SIGTERM "
      "停止）。还没有客户端鉴权：\n"
      "                                           监听非本机地址时需要 --allow-unauthenticated\n"
      "  llmgw check [-c 配置文件] [--skip-env]   校验配置文件\n"
      "  llmgw plugins                           列出可以在 pipeline 中启用的中间件插件\n"
      "  llmgw version                           显示版本\n"
      "\n"
      "check 的退出码：0 配置有效（可能有警告），1 配置有错误，2 参数错误或文件读不了\n"
      "--skip-env：不检查环境变量中的密钥，用于在没有密钥的环境（比如 CI）中校验配置文件\n",
      stderr);
}

int check(const std::vector<std::string_view>& args) {
  std::string path = "llmgw.json";
  llmgw::config::LoadOptions options;
  for (std::size_t i = 0; i < args.size(); ++i) {
    if ((args[i] == "-c" || args[i] == "--config") && i + 1 < args.size()) {
      path = std::string(args[++i]);
    } else if (args[i] == "--skip-env") {
      options.resolve_env = false;
    } else {
      std::fprintf(stderr, "未知参数：%s\n\n", std::string(args[i]).c_str());
      print_usage();
      return kUsageError;
    }
  }

  const auto report = llmgw::app::load_config(path, options, *llmgw::app::builtin_registry());
  const bool unreadable =
      !report.ok() && report.issues.size() == 1 && report.issues[0].path == path;
  if (report.ok()) {
    std::printf("%s：配置有效（%zu 个上游，%zu 个模型", path.c_str(),
                report.snapshot->providers.size(), report.snapshot->models.size());
    if (report.warning_count() > 0) std::printf("，%zu 个警告", report.warning_count());
    std::printf("）%s\n", options.resolve_env ? "" : "；未检查环境变量中的密钥");
  } else {
    std::printf("%s：%zu 个错误，%zu 个警告\n", path.c_str(), report.error_count(),
                report.warning_count());
  }
  for (const auto& issue : report.issues) {
    const char* level = issue.severity == llmgw::config::Severity::kError ? "错误" : "警告";
    std::printf("  %s  %s：%s\n", level, issue.path.c_str(), issue.message.c_str());
  }
  if (unreadable) return kUsageError;
  return report.ok() ? kOk : kConfigError;
}

}  // namespace

int main(int argc, char** argv) {
  const std::vector<std::string_view> args(argv + 1, argv + argc);
  if (args.empty() || args[0] == "help" || args[0] == "-h" || args[0] == "--help") {
    print_usage();
    return args.empty() ? kUsageError : kOk;
  }
  if (args[0] == "check") return check({args.begin() + 1, args.end()});
  if (args[0] == "serve") return llmgw::app::serve({args.begin() + 1, args.end()});
  if (args[0] == "plugins" && args.size() == 1) {
    return llmgw::app::list_plugins(*llmgw::app::builtin_registry());
  }
  if (args[0] == "version" || args[0] == "--version") {
    std::printf("llmgw %.*s\n", static_cast<int>(llmgw::core::kVersion.size()),
                llmgw::core::kVersion.data());
    return kOk;
  }
  std::fprintf(stderr, "未知命令：%s\n\n", std::string(args[0]).c_str());
  print_usage();
  return kUsageError;
}
