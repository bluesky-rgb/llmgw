#include "config.h"

#include <cstdlib>
#include <fstream>
#include <set>
#include <sstream>
#include <stdexcept>

#include <nlohmann/json.hpp>

#include "url.h"

namespace llmgw {

using nlohmann::json;

const ProviderConfig* Config::find_provider(const std::string& name) const {
  for (const auto& p : providers) {
    if (p.name == name) return &p;
  }
  return nullptr;
}

const ModelRoute* Config::find_model(const std::string& name) const {
  for (const auto& m : models) {
    if (m.name == name) return &m;
  }
  return nullptr;
}

namespace {

[[noreturn]] void fail(const std::string& msg) {
  throw std::runtime_error("配置错误: " + msg);
}

std::string require_string(const json& obj, const char* key, const std::string& where) {
  if (!obj.contains(key) || !obj[key].is_string() || obj[key].get<std::string>().empty()) {
    fail(where + " 缺少字符串字段 \"" + key + "\"");
  }
  return obj[key].get<std::string>();
}

ProviderConfig parse_provider(const json& j, size_t index) {
  const std::string where = "providers[" + std::to_string(index) + "]";
  if (!j.is_object()) fail(where + " 必须是对象");

  ProviderConfig p;
  p.name = require_string(j, "name", where);
  p.base_url = require_string(j, "base_url", where);
  try {
    parse_base_url(p.base_url);
  } catch (const std::exception& e) {
    fail(where + " 的 base_url 无效: " + e.what());
  }

  // api_key 优先直接读取，其次从环境变量读取（推荐，避免把密钥写进配置文件）
  if (j.contains("api_key")) {
    p.api_key = j.at("api_key").get<std::string>();
  } else if (j.contains("api_key_env")) {
    const auto env_name = j.at("api_key_env").get<std::string>();
    if (const char* v = std::getenv(env_name.c_str())) p.api_key = v;
  }

  p.connect_timeout_seconds = j.value("connect_timeout_seconds", p.connect_timeout_seconds);
  p.read_timeout_seconds = j.value("read_timeout_seconds", p.read_timeout_seconds);
  if (p.connect_timeout_seconds <= 0 || p.read_timeout_seconds <= 0) {
    fail(where + " 的超时时间必须大于 0");
  }
  if (j.contains("headers")) {
    for (const auto& [k, v] : j.at("headers").items()) {
      p.extra_headers[k] = v.get<std::string>();
    }
  }
  return p;
}

ModelRoute parse_model(const json& j, size_t index) {
  const std::string where = "models[" + std::to_string(index) + "]";
  if (!j.is_object()) fail(where + " 必须是对象");

  ModelRoute m;
  m.name = require_string(j, "name", where);
  if (!j.contains("targets") || !j["targets"].is_array() || j["targets"].empty()) {
    fail(where + " 至少需要一个 targets");
  }
  size_t ti = 0;
  for (const auto& t : j["targets"]) {
    const std::string twhere = where + ".targets[" + std::to_string(ti++) + "]";
    Target target;
    target.provider = require_string(t, "provider", twhere);
    // 不写 model 时沿用对外模型名
    target.model = t.value("model", m.name);
    m.targets.push_back(std::move(target));
  }
  return m;
}

}  // namespace

Config parse_config(const std::string& json_text) {
  json j;
  try {
    j = json::parse(json_text);
  } catch (const json::parse_error& e) {
    fail(std::string("JSON 解析失败: ") + e.what());
  }
  if (!j.is_object()) fail("顶层必须是对象");

  Config cfg;
  try {
    if (j.contains("listen")) {
      const auto& l = j["listen"];
      cfg.listen.host = l.value("host", cfg.listen.host);
      cfg.listen.port = l.value("port", cfg.listen.port);
      cfg.listen.threads = l.value("threads", cfg.listen.threads);
    }
    if (cfg.listen.port < 0 || cfg.listen.port > 65535) fail("listen.port 超出范围");
    if (cfg.listen.threads <= 0) fail("listen.threads 必须大于 0");

    if (j.contains("api_keys")) {
      cfg.api_keys = j["api_keys"].get<std::vector<std::string>>();
    }

    if (!j.contains("providers") || !j["providers"].is_array() || j["providers"].empty()) {
      fail("至少需要配置一个 providers");
    }
    std::set<std::string> provider_names;
    for (size_t i = 0; i < j["providers"].size(); ++i) {
      auto p = parse_provider(j["providers"][i], i);
      if (!provider_names.insert(p.name).second) fail("provider 名称重复: " + p.name);
      cfg.providers.push_back(std::move(p));
    }

    if (!j.contains("models") || !j["models"].is_array() || j["models"].empty()) {
      fail("至少需要配置一个 models");
    }
    std::set<std::string> model_names;
    for (size_t i = 0; i < j["models"].size(); ++i) {
      auto m = parse_model(j["models"][i], i);
      if (!model_names.insert(m.name).second) fail("model 名称重复: " + m.name);
      for (const auto& t : m.targets) {
        if (!provider_names.count(t.provider)) {
          fail("model \"" + m.name + "\" 引用了不存在的 provider \"" + t.provider + "\"");
        }
      }
      cfg.models.push_back(std::move(m));
    }
  } catch (const json::exception& e) {
    fail(std::string("字段类型不正确: ") + e.what());
  }
  return cfg;
}

Config load_config_file(const std::string& path) {
  std::ifstream in(path);
  if (!in) throw std::runtime_error("无法打开配置文件: " + path);
  std::stringstream ss;
  ss << in.rdbuf();
  return parse_config(ss.str());
}

}  // namespace llmgw
