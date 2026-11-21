#pragma once

#include <map>
#include <string>
#include <vector>

namespace llmgw {

// 一个上游服务：OpenAI、DeepSeek、本地 llama.cpp 等任何兼容 OpenAI 接口的服务
struct ProviderConfig {
  std::string name;
  std::string base_url;   // 例如 https://api.openai.com/v1 或 http://127.0.0.1:8081/v1
  std::string api_key;    // 可以为空（例如本地 llama.cpp）
  int connect_timeout_seconds = 10;
  int read_timeout_seconds = 300;
  std::map<std::string, std::string> extra_headers;
};

// 一个对外模型名对应的某个上游目标
struct Target {
  std::string provider;   // ProviderConfig::name
  std::string model;      // 发给上游时使用的模型名
};

// 对外暴露的模型。targets 按顺序尝试，前一个失败时自动切到下一个
struct ModelRoute {
  std::string name;
  std::vector<Target> targets;
};

struct ListenConfig {
  std::string host = "0.0.0.0";
  int port = 8080;
  int threads = 64;  // 当前版本中一个流式请求会占用一个线程
};

struct Config {
  ListenConfig listen;
  std::vector<std::string> api_keys;  // 客户端访问网关的密钥；为空表示不鉴权
  std::vector<ProviderConfig> providers;
  std::vector<ModelRoute> models;

  const ProviderConfig* find_provider(const std::string& name) const;
  const ModelRoute* find_model(const std::string& name) const;
};

// 解析 JSON 文本。出错时抛出 std::runtime_error，错误信息说明具体哪里不对
Config parse_config(const std::string& json_text);
Config load_config_file(const std::string& path);

}  // namespace llmgw
