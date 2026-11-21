// 各部分的解析：server、timeouts、providers、proxy、models。
// 每个函数只负责自己这一部分的字段；跨部分的检查（引用关系、重名）在 loader.cpp。
#include "sections.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <string>
#include <utility>

namespace llmgw::config::detail {
namespace {

constexpr std::int64_t kMaxTimeoutSeconds = std::int64_t{24} * 3600;  // 一天

bool all_chars(std::string_view s, std::string_view extra) {
  return !s.empty() && std::all_of(s.begin(), s.end(), [&](char c) {
    return std::isalnum(static_cast<unsigned char>(c)) != 0 ||
           extra.find(c) != std::string_view::npos;
  });
}

std::optional<Seconds> read_seconds(ObjectReader& r, std::string_view key) {
  auto v = r.integer(key, 1, kMaxTimeoutSeconds);
  return v ? std::optional<Seconds>(Seconds(*v)) : std::nullopt;
}

std::optional<FieldPath> read_path(std::string_view text, const std::string& path,
                                   Diagnostics* diag) {
  auto p = parse_field_path(text);
  if (!p) {
    diag->error(path, p.error().message);
    return std::nullopt;
  }
  return *p;
}

std::optional<Url> read_url(ObjectReader& r, std::string_view key, bool required) {
  auto text = r.string(key, required);
  if (!text) return std::nullopt;
  auto url = parse_url(*text);
  if (!url) {
    r.diag()->error(r.path_of(key), url.error().message);
    return std::nullopt;
  }
  return *url;
}

std::optional<Endpoint> read_endpoint(ObjectReader& r, std::string_view key) {
  auto text = r.string(key);
  if (!text) return std::nullopt;
  auto ep = parse_endpoint(*text);
  if (!ep) {
    r.diag()->error(r.path_of(key), ep.error().message);
    return std::nullopt;
  }
  return *ep;
}

// 依次读取 object 中的每个键值对（rename_fields、set_fields、codes 等）
template <class F>
void for_each_entry(ObjectReader& parent, std::string_view key, F&& f) {
  const Json* v = parent.raw(key);
  if (v == nullptr) return;
  if (!v->is_object()) {
    parent.diag()->error(parent.path_of(key), "应该是一个对象 {…}，实际是" + type_name(*v));
    return;
  }
  for (const auto& [k, value] : v->items()) f(k, value, parent.path_of(key) + "." + k);
}

TimeoutOverrides read_overrides(ObjectReader r) {
  TimeoutOverrides o{};
  o.connect = read_seconds(r, "connect");
  o.first_byte = read_seconds(r, "first_byte");
  o.first_byte_non_stream = read_seconds(r, "first_byte_non_stream");
  o.idle = read_seconds(r, "idle");
  if (r.raw("total") != nullptr) {
    r.diag()->error(r.path_of("total"),
                    "总超时覆盖整个请求（包括切换到其他目标），只能在顶层 timeouts.total 中设置");
  }
  r.finish();
  return o;
}

}  // namespace

bool is_loopback(std::string_view host) {
  return host == "localhost" || host == "::1" || host.starts_with("127.");
}

Timeouts merge(const Timeouts& global, const TimeoutOverrides& o) {
  Timeouts t = global;
  if (o.connect) t.connect = *o.connect;
  if (o.first_byte) t.first_byte = *o.first_byte;
  if (o.first_byte_non_stream) t.first_byte_non_stream = *o.first_byte_non_stream;
  if (o.idle) t.idle = *o.idle;
  return t;
}

namespace {
void check_one(const char* name, Seconds value, Seconds total, const std::string& path,
               Diagnostics* diag) {
  if (value > total) {
    diag->error(path + "." + name, std::string(name) + "（" + std::to_string(value.count()) +
                                       " 秒）不能大于总超时 total（" +
                                       std::to_string(total.count()) + " 秒），否则永远不会生效");
  }
}
}  // namespace

void check_against_total(const Timeouts& t, const std::string& path, Diagnostics* diag) {
  check_one("connect", t.connect, t.total, path, diag);
  check_one("first_byte", t.first_byte, t.total, path, diag);
  check_one("first_byte_non_stream", t.first_byte_non_stream, t.total, path, diag);
  check_one("idle", t.idle, t.total, path, diag);
}

void check_overrides_against_total(const TimeoutOverrides& o, Seconds total,
                                   const std::string& path, Diagnostics* diag) {
  if (o.connect) check_one("connect", *o.connect, total, path, diag);
  if (o.first_byte) check_one("first_byte", *o.first_byte, total, path, diag);
  if (o.first_byte_non_stream)
    check_one("first_byte_non_stream", *o.first_byte_non_stream, total, path, diag);
  if (o.idle) check_one("idle", *o.idle, total, path, diag);
}

ServerConfig read_server(ObjectReader r) {
  ServerConfig s;
  if (auto v = r.string("host")) s.host = *v;
  if (auto v = r.integer("port", 1, 65535)) s.port = static_cast<std::uint16_t>(*v);
  if (auto v = read_endpoint(r, "admin_listen")) {
    s.admin = *v;
    if (!is_loopback(v->host)) {
      r.diag()->warning(
          r.path_of("admin_listen"),
          "管理接口监听在 \"" + v->host + "\"，会暴露在网络上；除非确实需要，请使用 127.0.0.1");
    }
  }
  if (auto v = read_endpoint(r, "observability_listen")) s.observability = *v;
  if (auto v = r.integer("workers", 0, 1024)) s.workers = static_cast<unsigned>(*v);
  if (auto v = r.integer("max_connections", 1, 10'000'000)) {
    s.max_connections = static_cast<std::uint32_t>(*v);
  }
  r.finish();

  const std::array<std::pair<const char*, std::uint16_t>, 3> ports{
      {{"port", s.port},
       {"admin_listen", s.admin.port},
       {"observability_listen", s.observability.port}}};
  for (std::size_t i = 0; i < ports.size(); ++i) {
    for (std::size_t j = i + 1; j < ports.size(); ++j) {
      if (ports[i].second == ports[j].second) {
        r.diag()->error(r.path_of(ports[j].first), "端口 " + std::to_string(ports[j].second) +
                                                       " 与 " + r.path_of(ports[i].first) +
                                                       " 相同");
      }
    }
  }
  return s;
}

Timeouts read_timeouts(ObjectReader r) {
  Timeouts t;
  if (auto v = read_seconds(r, "connect")) t.connect = *v;
  if (auto v = read_seconds(r, "first_byte")) t.first_byte = *v;
  if (auto v = read_seconds(r, "first_byte_non_stream")) t.first_byte_non_stream = *v;
  if (auto v = read_seconds(r, "idle")) t.idle = *v;
  if (auto v = read_seconds(r, "total")) t.total = *v;
  r.finish();
  check_against_total(t, r.path(), r.diag());
  return t;
}

Provider read_provider(ObjectReader r) {
  Provider p;
  if (auto v = r.string("name", true)) {
    p.name = *v;
    if (!all_chars(p.name, "-_")) {
      r.diag()->error(r.path_of("name"),
                      "上游名 \"" + p.name + "\" 只能包含字母、数字、- 和 _（它会用作指标标签）");
    }
  }
  if (auto v = read_url(r, "base_url", true)) {
    p.base_url = *v;
    if (v->scheme == "http" && !is_loopback(v->host)) {
      r.diag()->warning(r.path_of("base_url"),
                        "使用 http，密钥会明文传输；除非是本机或可信的内网，请使用 https");
    }
  }
  if (auto v = r.string("api_key_env", true)) {
    p.api_key_env = *v;
    if (!all_chars(p.api_key_env, "_")) {
      r.diag()->error(
          r.path_of("api_key_env"),
          "应该是环境变量的名字（比如 OPENAI_API_KEY），而不是密钥本身；只能包含字母、数字和 _");
    }
  }
  if (auto v = r.integer("max_connections", 1, 100'000))
    p.max_connections = static_cast<std::uint32_t>(*v);
  if (auto m = r.object("error_mapping")) {
    ErrorMapping em;
    if (auto f = m->string("field", true)) {
      if (auto fp = read_path(*f, m->path_of("field"), r.diag())) em.field = *fp;
    }
    if (m->raw("codes") == nullptr)
      m->string("codes", true);  // 登记为缺少的必填字段，在 finish() 中报告
    for_each_entry(
        *m, "codes", [&](const std::string& code, const Json& v, const std::string& path) {
          if (!v.is_number_integer() || v.get<std::int64_t>() < 400 ||
              v.get<std::int64_t>() > 599) {
            r.diag()->error(path, "应该是 400–599 之间的 HTTP 状态码，实际是 " + v.dump());
            return;
          }
          em.codes[code] = v.get<int>();
        });
    m->finish();
    p.error_mapping = std::move(em);
  }
  r.unsupported("quota", "将在 v0.3 提供，任务 2.7");
  r.finish();
  return p;
}

ProxyConfig read_proxy(ObjectReader r, const EnvLookup& getenv) {
  ProxyConfig proxy;
  if (auto v = r.string("https")) {
    std::string text = *v;
    std::string source = "\"" + text + "\"";
    if (text == "env") {
      auto env = getenv("HTTPS_PROXY");
      if (!env || env->empty()) env = getenv("https_proxy");
      text = env.value_or("");
      source = "环境变量 HTTPS_PROXY 的值 \"" + text + "\"";
    }
    if (!text.empty()) {
      if (auto url = parse_url(text)) {
        proxy.https = *url;
      } else {
        r.diag()->error(r.path_of("https"), source + "：" + url.error().message);
      }
    }
  }
  for_each_entry(r, "per_provider",
                 [&](const std::string& name, const Json& v, const std::string& path) {
                   if (!v.is_string()) {
                     r.diag()->error(path, "应该是代理地址字符串，实际是" + type_name(v));
                     return;
                   }
                   if (auto url = parse_url(v.get<std::string>())) {
                     proxy.per_provider[name] = *url;
                   } else {
                     r.diag()->error(path, url.error().message);
                   }
                 });
  r.finish();
  return proxy;
}

TargetConfig read_target(ObjectReader r, const Timeouts& global) {
  TargetConfig t;
  if (auto v = r.string("provider", true)) t.provider = *v;
  if (auto v = r.string("model", true)) t.model = *v;
  if (const Json* arr = r.array("drop_fields")) {
    for (std::size_t i = 0; i < arr->size(); ++i) {
      const auto path = index_path(r.path_of("drop_fields"), i);
      if (!(*arr)[i].is_string()) {
        r.diag()->error(path, "应该是字段路径字符串，比如 \"messages[].reasoning_content\"");
        continue;
      }
      if (auto fp = read_path((*arr)[i].get<std::string>(), path, r.diag()))
        t.drop_fields.push_back(*fp);
    }
  }
  for_each_entry(r, "rename_fields",
                 [&](const std::string& from, const Json& v, const std::string& path) {
                   auto f = read_path(from, path, r.diag());
                   if (!v.is_string()) {
                     r.diag()->error(path, "应该是新的字段名（字符串），实际是" + type_name(v));
                     return;
                   }
                   auto to = read_path(v.get<std::string>(), path, r.diag());
                   if (f && to) t.rename_fields.push_back({.from = *f, .to = *to});
                 });
  for (const auto& a : t.rename_fields) {
    for (const auto& b : t.rename_fields) {
      if (a.to == b.from) {
        r.diag()->error(
            r.path_of("rename_fields"),
            "\"" + to_string(b.from) +
                "\" 既是改名的目标又是改名的来源，结果取决于执行顺序；请合并成一次改名");
      }
    }
  }
  for_each_entry(r, "set_fields",
                 [&](const std::string& key, const Json& v, const std::string& path) {
                   if (auto fp = read_path(key, path, r.diag()))
                     t.set_fields.push_back({.path = *fp, .json_value = v.dump()});
                 });
  TimeoutOverrides o{};
  if (auto ov = r.object("timeouts")) o = read_overrides(*ov);
  t.timeouts = merge(global, o);
  // 只检查目标自己覆盖的项；继承来的全局值已经在 timeouts 部分检查过，不重复报告
  check_overrides_against_total(o, global.total, r.path_of("timeouts"), r.diag());
  r.finish();
  return t;
}

ModelConfig read_model(ObjectReader r, const Timeouts& global) {
  ModelConfig m;
  if (auto v = r.string("name", true)) {
    m.name = *v;
    if (!all_chars(m.name, "._:/-")) {
      r.diag()->error(
          r.path_of("name"),
          "模型名 \"" + m.name + "\" 只能包含字母、数字和 . _ : / -（它会用作指标标签）");
    }
  }
  const Json* targets = r.array("targets", true);
  if (targets != nullptr && targets->empty()) {
    r.diag()->error(r.path_of("targets"), "至少需要一个目标");
  }
  if (targets != nullptr) {
    for (std::size_t i = 0; i < targets->size(); ++i) {
      ObjectReader tr((*targets)[i], index_path(r.path_of("targets"), i), r.diag());
      if (tr.valid()) m.targets.push_back(read_target(std::move(tr), global));
    }
  }
  r.finish();
  return m;
}

}  // namespace llmgw::config::detail
