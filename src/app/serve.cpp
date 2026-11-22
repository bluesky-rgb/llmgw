#include "serve.h"

#include <pthread.h>

#include <csignal>
#include <cstdio>
#include <string>

#include "gateway.h"
#include "llmgw/config/loader.h"
#include "llmgw/server/server.h"
#include "llmgw/upstream/client.h"

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

}  // namespace

int serve(const std::vector<std::string_view>& args) {
  std::string path = "llmgw.json";
  for (std::size_t i = 0; i < args.size(); ++i) {
    if ((args[i] == "-c" || args[i] == "--config") && i + 1 < args.size()) {
      path = std::string(args[++i]);
    } else {
      std::fprintf(stderr, "未知参数：%s\n", std::string(args[i]).c_str());
      return kUsageError;
    }
  }

  const auto report = config::load_file(path, config::LoadOptions{});
  if (!report.ok()) {
    const bool unreadable = report.issues.size() == 1 && report.issues[0].path == path;
    std::fprintf(stderr, "%s：配置有错误，没有启动\n", path.c_str());
    print_issues(report);
    return unreadable ? kUsageError : kStartError;
  }
  print_issues(report);  // 只有警告
  const std::shared_ptr<const config::Snapshot> snapshot = report.snapshot;

  auto tls = upstream::make_tls_contexts(*snapshot);  // 加载系统证书库约 40 ms，只做一次
  if (!tls) {
    std::fprintf(stderr, "TLS 初始化失败：%s\n", tls.error().message.c_str());
    return kStartError;
  }

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
      .dispatcher_factory = make_dispatcher_factory(snapshot, *tls),
      .metrics_factory = {},
  });
  if (auto r = server.start(); !r) {
    std::fprintf(stderr, "启动失败：%s\n", r.error().message.c_str());
    return kStartError;
  }
  // 测试和运维脚本从这一行读取实际的端口（配置中写 0 时由系统分配）
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

}  // namespace llmgw::app
