// 顶层的加载流程与跨部分的校验（引用关系、重名、未使用的上游）。
#include "llmgw/config/loader.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <set>
#include <sstream>
#include <utility>

#include "json_reader.h"
#include "sections.h"

namespace llmgw::config {
namespace {

using detail::Diagnostics;
using detail::index_path;
using detail::Json;
using detail::ObjectReader;

std::optional<std::string> real_getenv(const std::string& name) {
  // 只在加载时（单线程的启动阶段）调用；请求处理中不访问环境变量
  const char* v = std::getenv(name.c_str());  // NOLINT(concurrency-mt-unsafe)
  return v == nullptr ? std::nullopt : std::optional<std::string>(v);
}

// 把字节位置换算成“第 N 行第 M 列”
std::string line_col(std::string_view text, std::size_t byte) {
  std::size_t line = 1;
  std::size_t col = 1;
  for (std::size_t i = 0; i < byte && i < text.size(); ++i) {
    if (text[i] == '\n') {
      ++line;
      col = 1;
    } else {
      ++col;
    }
  }
  return "第 " + std::to_string(line) + " 行第 " + std::to_string(col) + " 列";
}

template <class T, class F>
void read_array(ObjectReader& top, std::string_view key, std::vector<T>* out, F&& read_one) {
  const Json* arr = top.array(key, true);
  if (arr == nullptr) return;
  if (arr->empty()) top.diag()->error(top.path_of(key), "至少需要一项");
  for (std::size_t i = 0; i < arr->size(); ++i) {
    ObjectReader r((*arr)[i], index_path(std::string(key), i), top.diag());
    if (r.valid()) out->push_back(read_one(std::move(r)));
  }
}

// pipeline：每一项是插件名（字符串），或者 {"name": ..., "config": {...}}（任务 2.1）
void read_pipeline(ObjectReader& top, std::vector<PipelineEntry>* out) {
  const Json* arr = top.array("pipeline");
  if (arr == nullptr) return;
  for (std::size_t i = 0; i < arr->size(); ++i) {
    const Json& item = (*arr)[i];
    PipelineEntry entry{.name = {}, .config_json = "{}", .path = index_path("pipeline", i)};
    if (item.is_string()) {
      entry.name = item.get<std::string>();
    } else if (item.is_object()) {
      ObjectReader r(item, entry.path, top.diag());
      entry.name = r.string("name", true).value_or("");
      // 内容由插件校验；这里只要求它是对象
      if (r.object("config")) entry.config_json = r.raw("config")->dump();
      r.finish();
    } else {
      top.diag()->error(entry.path,
                        "应该是插件名（字符串）或者 {\"name\": ..., \"config\": {...}}，实际是" +
                            detail::type_name(item));
      continue;
    }
    if (entry.name.empty()) {
      // 对象形式缺少 name 时 finish() 已经报告；写了但是空字符串时在这里报告
      if (item.is_string()) {
        top.diag()->error(entry.path, "插件名不能为空");
      } else if (const auto it = item.find("name"); it != item.end() && it->is_string()) {
        top.diag()->error(entry.path + ".name", "插件名不能为空");
      }
      continue;
    }
    // 同一个插件出现两次：执行顺序和计数（限流、并发）都会有歧义
    const auto same = std::ranges::find(*out, entry.name, &PipelineEntry::name);
    if (same != out->end()) {
      top.diag()->error(entry.path, "插件 \"" + entry.name + "\" 与 " + same->path + " 重复");
      continue;
    }
    out->push_back(std::move(entry));
  }
}

// 名字唯一
template <class T>
void check_unique(const std::vector<T>& items, const char* section, const char* what,
                  Diagnostics* diag) {
  for (std::size_t i = 0; i < items.size(); ++i) {
    for (std::size_t j = 0; j < i; ++j) {
      if (!items[i].name.empty() && items[i].name == items[j].name) {
        diag->error(index_path(section, i) + ".name", std::string(what) + " \"" + items[i].name +
                                                          "\" 与 " + index_path(section, j) +
                                                          " 重名");
        break;
      }
    }
  }
}

std::vector<std::string> provider_names(const Snapshot& s) {
  std::vector<std::string> names;
  names.reserve(s.providers.size());
  for (const auto& p : s.providers) names.push_back(p.name);
  return names;
}

// 一个模型的目标：引用的上游必须存在；同一个模型中不应该有完全相同的两个目标
void check_targets(const Snapshot& s, std::size_t m, const std::vector<std::string>& names,
                   Diagnostics* diag) {
  const auto& targets = s.models[m].targets;
  const auto base = index_path("models", m) + ".targets";
  for (std::size_t t = 0; t < targets.size(); ++t) {
    const auto& target = targets[t];
    if (target.provider.empty()) continue;  // 缺少字段，已经报告过
    if (s.find_provider(target.provider) == nullptr) {
      diag->error(index_path(base, t) + ".provider",
                  "上游 \"" + target.provider + "\" 没有定义" +
                      detail::did_you_mean(target.provider, names));
    }
    for (std::size_t k = 0; k < t; ++k) {
      if (targets[k].provider == target.provider && targets[k].model == target.model) {
        diag->warning(
            index_path(base, t),
            "与 " + index_path(base, k) + " 完全相同（同一个上游和模型名），多半是复制粘贴的错误");
        break;
      }
    }
  }
}

void check_unused_providers(const Snapshot& s, Diagnostics* diag) {
  std::set<std::string> used;
  for (const auto& m : s.models) {
    for (const auto& t : m.targets) used.insert(t.provider);
  }
  for (std::size_t i = 0; i < s.providers.size(); ++i) {
    if (!s.providers[i].name.empty() && !used.contains(s.providers[i].name)) {
      diag->warning(index_path("providers", i),
                    "上游 \"" + s.providers[i].name + "\" 没有被任何模型使用");
    }
  }
}

void check_references(const Snapshot& s, Diagnostics* diag) {
  const auto names = provider_names(s);
  for (std::size_t m = 0; m < s.models.size(); ++m) check_targets(s, m, names, diag);
  check_unused_providers(s, diag);
  for (const auto& [name, url] : s.proxy.per_provider) {
    if (s.find_provider(name) == nullptr) {
      diag->error("proxy.per_provider." + name,
                  "上游 \"" + name + "\" 没有定义" + detail::did_you_mean(name, names));
    }
  }
}

void resolve_keys(Snapshot* s, const EnvLookup& getenv, Diagnostics* diag) {
  for (std::size_t i = 0; i < s->providers.size(); ++i) {
    auto& p = s->providers[i];
    if (p.api_key_env.empty()) continue;  // 缺少字段，已经报告过
    auto v = getenv(p.api_key_env);
    if (!v || v->empty()) {
      diag->error(
          index_path("providers", i) + ".api_key_env",
          "环境变量 " + p.api_key_env +
              " 不存在或为空。请设置它；只想检查配置文件格式时，可以用 llmgw check --skip-env");
      continue;
    }
    p.api_key = std::move(*v);
  }
}

}  // namespace

const Provider* Snapshot::find_provider(std::string_view name) const {
  auto it = std::find_if(providers.begin(), providers.end(),
                         [&](const Provider& p) { return p.name == name; });
  return it == providers.end() ? nullptr : &*it;
}

const ModelConfig* Snapshot::find_model(std::string_view name) const {
  auto it = std::find_if(models.begin(), models.end(),
                         [&](const ModelConfig& m) { return m.name == name; });
  return it == models.end() ? nullptr : &*it;
}

const ProxyUrl* Snapshot::proxy_for(const Provider& provider) const {
  if (auto it = proxy.per_provider.find(provider.name); it != proxy.per_provider.end()) {
    // 单独配置（包括 "direct"）优先
    const std::optional<ProxyUrl>& route = it->second;
    if (!route.has_value()) return nullptr;
    return &*route;
  }
  if (!proxy.https) return nullptr;
  for (const auto& entry : proxy.no_proxy) {
    if (no_proxy_matches(entry, provider.base_url.host)) return nullptr;
  }
  return &proxy.https.value();
}

std::size_t LoadReport::error_count() const {
  return static_cast<std::size_t>(std::count_if(
      issues.begin(), issues.end(), [](const Issue& i) { return i.severity == Severity::kError; }));
}

std::size_t LoadReport::warning_count() const { return issues.size() - error_count(); }

LoadReport parse(std::string_view json_text, const LoadOptions& options) {
  const EnvLookup getenv = options.getenv ? options.getenv : EnvLookup(real_getenv);
  Diagnostics diag;
  LoadReport report;

  if (json_text.starts_with("\xEF\xBB\xBF")) json_text.remove_prefix(3);  // UTF-8 BOM
  Json root;
  try {
    root = Json::parse(json_text);
  } catch (const Json::parse_error& e) {
    std::string detail = e.what();
    if (auto pos = detail.find(": "); pos != std::string::npos) detail = detail.substr(pos + 2);
    diag.error(line_col(json_text, e.byte > 0 ? e.byte - 1 : 0), "JSON 语法错误：" + detail);
    report.issues = std::move(diag.issues());
    return report;
  } catch (const Json::exception& e) {
    // 比如数字溢出（1e999）：nlohmann 抛出的是 out_of_range，不带位置信息（模糊测试发现）
    std::string detail = e.what();
    if (auto pos = detail.find("] "); pos != std::string::npos) detail = detail.substr(pos + 2);
    diag.error("（整个文件）", "JSON 中的数字超出范围或无法表示：" + detail);
    report.issues = std::move(diag.issues());
    return report;
  }

  auto snapshot = std::make_shared<Snapshot>();
  ObjectReader top(root, "", &diag);
  if (top.valid()) {
    if (auto r = top.object("server")) snapshot->server = detail::read_server(std::move(*r));
    if (auto r = top.object("timeouts")) snapshot->timeouts = detail::read_timeouts(std::move(*r));
    read_array(top, "providers", &snapshot->providers,
               [](ObjectReader r) { return detail::read_provider(std::move(r)); });
    if (auto r = top.object("proxy")) {
      snapshot->proxy = detail::read_proxy(std::move(*r), getenv, options.resolve_env);
    }
    if (auto r = top.object("upstream")) snapshot->upstream = detail::read_upstream(std::move(*r));
    if (auto r = top.object("retry")) snapshot->retry = detail::read_retry(std::move(*r));
    const Timeouts global = snapshot->timeouts;
    read_array(top, "models", &snapshot->models,
               [&](ObjectReader r) { return detail::read_model(std::move(r), global); });
    read_pipeline(top, &snapshot->pipeline);
    top.unsupported("keys", "将在 v0.3 提供，任务 2.2", "目前网关不做鉴权，请只在可信网络中使用");
    top.unsupported("pricing", "将在 v0.3 提供，任务 2.4");
    top.unsupported("accounting", "将在 v0.3 提供，任务 2.4");
    top.finish();

    check_unique(snapshot->providers, "providers", "上游", &diag);
    check_unique(snapshot->models, "models", "模型", &diag);
    check_references(*snapshot, &diag);
    if (options.resolve_env) resolve_keys(snapshot.get(), getenv, &diag);
  }

  if (!diag.has_errors()) report.snapshot = std::move(snapshot);
  report.issues = std::move(diag.issues());
  return report;
}

LoadReport load_file(const std::string& path, const LoadOptions& options) {
  const std::ifstream in(path, std::ios::binary);
  if (!in) {
    LoadReport report;
    report.issues.push_back({.severity = Severity::kError,
                             .path = path,
                             .message = "无法读取配置文件（文件不存在或没有权限）"});
    return report;
  }
  std::ostringstream text;
  text << in.rdbuf();
  return parse(text.str(), options);
}

}  // namespace llmgw::config
