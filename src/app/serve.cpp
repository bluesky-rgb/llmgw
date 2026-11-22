#include "serve.h"

#include <pthread.h>

#include <csignal>
#include <cstdio>
#include <memory>
#include <optional>
#include <string>

#include "gateway.h"
#include "llmgw/config/loader.h"
#include "llmgw/server/server.h"
#include "llmgw/upstream/client.h"
#include "plugins.h"

namespace llmgw::app {
namespace {

constexpr int kOk = 0;
constexpr int kStartError = 1;
constexpr int kUsageError = 2;

void print_issues(const config::LoadReport& report) {
  for (const auto& issue : report.issues) {
    const char* level = issue.severity == config::Severity::kError ? "错误" : "警告";
    std::fprintf(stderr, "  %s  %s：%s\n", level, issue.path.c_str(), issue.message.c_str());
  }
}

struct Args {
  std::string path = "llmgw.json";
  bool allow_unauthenticated = false;
};

// 有值：解析成功；没有值：参数错误（已经打印）
std::optional<Args> parse_args(const std::vector<std::string_view>& args) {
  Args out;
  for (std::size_t i = 0; i < args.size(); ++i) {
    if ((args[i] == "-c" || args[i] == "--config") && i + 1 < args.size()) {
      out.path = std::string(args[++i]);
    } else if (args[i] == "--allow-unauthenticated") {
      out.allow_unauthenticated = true;
    } else {
      std::fprintf(stderr, "未知参数：%s\n", std::string(args[i]).c_str());
      return std::nullopt;
    }
  }
  return out;
}

// 网关持有上游的 key：数据面监听非本机地址而没有客户端鉴权时，任何能访问这个端口的人都能用这些 key
// 调用模型、产生费用（任务 1.9）。所以没有启用 auth 插件时默认拒绝启动，需要显式确认；启用了 auth
// （任务 2.2）就可以直接监听。按实际要绑定的地址判断（解析之后），解析失败也当作“不是本机地址”
// （独立审查 1.9 #1）
bool allowed_to_listen(const config::Snapshot& snapshot, bool allow_unauthenticated) {
  if (has_auth(snapshot)) return true;
  if (server::listen_address_is_loopback(snapshot.server.host).value_or(false)) return true;
  if (!allow_unauthenticated) {
    std::fprintf(
        stderr,
        "拒绝启动：数据面监听 %s（不是本机地址），而 pipeline 中没有启用客户端鉴权（auth 插件）。\n"
        "任何能访问这个端口的人都能用配置中的上游 key 调用模型。\n"
        "启用鉴权：在配置中加上 \"pipeline\": [\"auth\"] 和 keys（llmgw keygen 生成）；"
        "只在本机使用：把 server.host 改为 127.0.0.1；确认网络可信（比如容器网络、防火墙后面）："
        "加 --allow-unauthenticated\n",
        snapshot.server.host.c_str());
    return false;
  }
  std::fprintf(stderr,
               "警告：数据面监听 %s 且没有客户端鉴权：请确认只有可信的客户端能访问这个端口\n",
               snapshot.server.host.c_str());
  return true;
}

// 启动 Server，等到 SIGINT / SIGTERM 再停止
int run(const std::shared_ptr<const config::Snapshot>& snapshot,
        const std::shared_ptr<const upstream::TlsContexts>& tls,
        const std::shared_ptr<const middleware::Registry>& registry) {
  // 必须在创建任何线程之前：扩容 fd 表；屏蔽 SIGINT / SIGTERM（新线程继承信号掩码，
  // 信号只由下面的 sigwait 接收，不会打断 Worker 线程）
  server::pregrow_fd_table();
  // 标准错误是一个已关闭的管道时（比如被 | head 截断），写日志会触发 SIGPIPE 杀死进程；
  // socket 的写入用的是 MSG_NOSIGNAL，不受影响（独立审查 1.7 #7）
  std::signal(SIGPIPE, SIG_IGN);  // NOLINT(cert-err33-c)：返回值只有 SIG_ERR 一种失败，这里无法处理
  sigset_t signals;
  sigemptyset(&signals);
  sigaddset(&signals, SIGINT);
  sigaddset(&signals, SIGTERM);
  pthread_sigmask(SIG_BLOCK, &signals, nullptr);

  server::Server server(server::ServerOptions{
      .config = snapshot,
      .dispatcher_factory = make_dispatcher_factory(snapshot, tls),
      .metrics_factory = {},
      .chain_factory = make_chain_factory(snapshot, registry),
  });
  if (auto r = server.start(); !r) {
    std::fprintf(stderr, "启动失败：%s\n", r.error().message.c_str());
    return kStartError;
  }
  // 测试和运维脚本从这一行读取实际的端口
  std::fprintf(stderr, "llmgw 已启动：数据面 %s:%u，管理面 %s:%u，观测面 %s:%u，%u 个 Worker\n",
               snapshot->server.host.c_str(), server.data_port(),
               snapshot->server.admin.host.c_str(), server.admin_port(),
               snapshot->server.observability.host.c_str(), server.observability_port(),
               server.workers());
  std::fflush(stderr);

  int received = 0;
  sigwait(&signals, &received);
  std::fprintf(stderr, "收到信号 %d，正在停止（进行中的请求会被中断；优雅退出见任务 3.3）\n",
               received);
  server.stop();
  std::fprintf(stderr, "已停止\n");
  return kOk;
}

}  // namespace

int serve(const std::vector<std::string_view>& args) {
  const auto parsed = parse_args(args);
  if (!parsed) return kUsageError;
  const auto registry = builtin_registry();
  const auto report = load_config(parsed->path, config::LoadOptions{}, *registry);
  if (!report.ok()) {
    const bool unreadable = report.issues.size() == 1 && report.issues[0].path == parsed->path;
    std::fprintf(stderr, "%s：配置有错误，没有启动\n", parsed->path.c_str());
    print_issues(report);
    return unreadable ? kUsageError : kStartError;
  }
  print_issues(report);  // 只有警告
  if (!allowed_to_listen(*report.snapshot, parsed->allow_unauthenticated)) return kStartError;
  auto tls = upstream::make_tls_contexts(*report.snapshot);  // 加载系统证书库约 40 ms，只做一次
  if (!tls) {
    std::fprintf(stderr, "TLS 初始化失败：%s\n", tls.error().message.c_str());
    return kStartError;
  }
  return run(report.snapshot, *tls, registry);
}

}  // namespace llmgw::app
