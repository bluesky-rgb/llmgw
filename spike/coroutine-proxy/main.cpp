// 协程 demo：每核一个线程 + SO_REUSEPORT + 每条连接一个协程的流式代理。
// 用法见 README.md；设计与验证见 docs/tasks/0.6-coroutine-demo.md。
#include <fcntl.h>
#include <sys/resource.h>
#include <unistd.h>

#include <charconv>
#include <cstdio>
#include <iostream>
#include <memory>
#include <thread>
#include <vector>

#include "common.h"
#include "worker.h"

namespace spike {

// 预先把进程的文件描述符表扩到上限。
// Linux 按 2 的幂扩容 fd 表；多线程进程每次扩容都要等一个 RCU 宽限期（几到十几毫秒），
// 期间调用线程被阻塞。不预扩容的话，建连高峰期每跨过一个 2 的幂就卡一次（见任务 0.7）。
// 代价：每个 fd 槽约 8 字节（上限 20000 时约 160 KB）；上限超过 2^20 时只扩到 2^20。
// 用 F_DUPFD 而不是 dup2：dup2 会先关闭已经打开的目标 fd，F_DUPFD 只取不小于 top 的空闲 fd。
void pregrow_fd_table() {
  rlimit rl{};
  if (getrlimit(RLIMIT_NOFILE, &rl) != 0 || rl.rlim_cur < 2) return;
  const int top = static_cast<int>(std::min<rlim_t>(rl.rlim_cur, 1 << 20)) - 1;
  const int fd = fcntl(STDERR_FILENO, F_DUPFD_CLOEXEC, top);
  if (fd >= 0) close(fd);
}

bool parse_seconds(std::string_view s, std::chrono::seconds& out) {
  if (!s.empty() && s.back() == 's') s.remove_suffix(1);
  long v = 0;
  auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
  if (ec != std::errc{} || p != s.data() + s.size() || v <= 0) return false;
  out = std::chrono::seconds(v);
  return true;
}

bool parse_args(int argc, char** argv, Options& opt) {
  for (int i = 1; i + 1 < argc; i += 2) {
    std::string_view key = argv[i], val = argv[i + 1];
    auto split = [](std::string_view hp, std::string& host, std::string& port) {
      auto pos = hp.rfind(':');
      if (pos == std::string_view::npos) return false;
      host = hp.substr(0, pos);
      port = hp.substr(pos + 1);
      return true;
    };
    if (key == "--listen") {
      std::string host, port;
      if (!split(val, host, port)) return false;
      opt.listen_host = host;
      opt.listen_port = static_cast<std::uint16_t>(std::stoi(port));
    } else if (key == "--upstream") {
      if (!split(val, opt.upstream_host, opt.upstream_port)) return false;
    } else if (key == "--workers") {
      opt.workers = static_cast<unsigned>(std::stoul(std::string(val)));
    } else if (key == "--total-timeout") {
      if (!parse_seconds(val, opt.total_timeout)) return false;
    } else if (key == "--pregrow-fds") {  // 对照实验用：--pregrow-fds off
      opt.no_pregrow_fds = (val == "off");
    } else if (key == "--idle-timeout") {
      if (!parse_seconds(val, opt.idle_timeout)) return false;
    } else {
      return false;
    }
  }
  return (argc % 2) == 1;
}

}  // namespace spike

int main(int argc, char** argv) {
  using namespace spike;
  Options opt;
  if (!parse_args(argc, argv, opt)) {
    std::cerr
        << "用法: coroutine-proxy [--listen 127.0.0.1:18100] [--upstream 127.0.0.1:18081]\n"
           "                       [--workers 0] [--total-timeout 600s] [--idle-timeout 60s]\n"
           "                       [--pregrow-fds off]\n";
    return 2;
  }
  if (!opt.no_pregrow_fds)
    pregrow_fd_table();  // 必须在启动任何线程之前（单线程时扩容不需要等 RCU）
  if (opt.workers == 0) opt.workers = std::max(1u, std::thread::hardware_concurrency());

  // 启动时解析一次上游地址（阻塞操作只在启动阶段发生）
  asio::io_context main_ioc;
  tcp::resolver resolver(main_ioc);
  auto upstream = resolver.resolve(opt.upstream_host, opt.upstream_port);

  static Stats stats;
  std::vector<std::unique_ptr<Worker>> workers;
  for (unsigned i = 0; i < opt.workers; ++i) {
    workers.push_back(std::make_unique<Worker>(opt, upstream, &stats));
  }
  for (auto& w : workers) w->start();
  std::fprintf(stderr, "coroutine-proxy: %u workers on %s:%u -> %s:%s\n", opt.workers,
               opt.listen_host.c_str(), opt.listen_port, opt.upstream_host.c_str(),
               opt.upstream_port.c_str());

  asio::signal_set signals(main_ioc, SIGINT, SIGTERM);
  signals.async_wait([&](auto, auto) { main_ioc.stop(); });
  main_ioc.run();
  for (auto& w : workers) w->stop();
  return 0;
}
