// 协程 demo：每核一个线程 + SO_REUSEPORT + 每条连接一个协程的流式代理。
// 用法见 README.md；设计与验证见 docs/tasks/0.6-coroutine-demo.md。
#include <charconv>
#include <cstdio>
#include <iostream>
#include <memory>
#include <thread>
#include <vector>

#include "common.h"
#include "worker.h"

namespace spike {

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
           "                       [--workers 0] [--total-timeout 600s] [--idle-timeout 60s]\n";
    return 2;
  }
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
