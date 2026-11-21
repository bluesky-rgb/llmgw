#include <csignal>
#include <cstring>
#include <iostream>
#include <string>

#include "config.h"
#include "gateway.h"
#include "logger.h"

namespace {

llmgw::Gateway* g_gateway = nullptr;

void on_signal(int) {
  if (g_gateway) g_gateway->stop();
}

void print_usage(const char* argv0) {
  std::cout << "用法: " << argv0 << " [--config 配置文件] [--check]\n"
            << "  --config, -c   配置文件路径，默认为 config.json\n"
            << "  --check        只检查配置是否正确，不启动服务\n"
            << "  --help, -h     显示帮助\n";
}

}  // namespace

int main(int argc, char** argv) {
  std::string config_path = "config.json";
  bool check_only = false;

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if ((arg == "--config" || arg == "-c") && i + 1 < argc) {
      config_path = argv[++i];
    } else if (arg == "--check") {
      check_only = true;
    } else if (arg == "--help" || arg == "-h") {
      print_usage(argv[0]);
      return 0;
    } else {
      std::cerr << "未知参数: " << arg << "\n";
      print_usage(argv[0]);
      return 2;
    }
  }

  llmgw::Config cfg;
  try {
    cfg = llmgw::load_config_file(config_path);
  } catch (const std::exception& e) {
    std::cerr << e.what() << "\n";
    return 1;
  }

  if (check_only) {
    std::cout << "配置正确: " << cfg.providers.size() << " 个 provider, " << cfg.models.size()
              << " 个模型\n";
    return 0;
  }

  for (const auto& p : cfg.providers) {
    if (p.api_key.empty()) {
      llmgw::log_event("info", "provider has no api key (fine for local servers)",
                       {{"provider", p.name}});
    }
  }

  const std::string host = cfg.listen.host;
  const int want_port = cfg.listen.port;
  llmgw::Gateway gateway(std::move(cfg));
  const int port = gateway.bind(host, want_port);
  if (port < 0) {
    std::cerr << "无法监听 " << host << ":" << want_port << "\n";
    return 1;
  }

  g_gateway = &gateway;
  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);
  std::signal(SIGPIPE, SIG_IGN);

  llmgw::log_event("info", "llmgw started", {{"host", host}, {"port", port}});
  gateway.run();
  llmgw::log_event("info", "llmgw stopped");
  g_gateway = nullptr;
  return 0;
}
