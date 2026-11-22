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

// drop_fields / rename_fields / set_fields 的路径（任务 1.6 补充的校验）：
// 最后一段不能带 []（删除、改名、设置的对象是一个字段，而不是数组中的每个元素本身）；
// 顶层的 model 和 stream 由网关管理（model 由路由决定，stream 决定了响应的解析方式），不能改写，
// 也不能改写它们下面的路径（model.x 会把 model 替换成对象）
std::optional<FieldPath> read_rewrite_path(std::string_view text, const std::string& path,
                                           Diagnostics* diag) {
  auto p = read_path(text, path, diag);
  if (!p) return std::nullopt;
  if (p->back().each) {
    diag->error(path,
                "字段路径 \"" + std::string(text) + "\" 的最后一段不能带 []：要改写的是一个字段");
    return std::nullopt;
  }
  if (p->front().name == "model" || p->front().name == "stream") {
    diag->error(path, "\"" + p->front().name +
                          "\" 由网关管理，不能改写（model 由路由决定，stream 决定响应的解析方式）");
    return std::nullopt;
  }
  return p;
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
  if (auto v = r.integer("max_body_bytes", 1024, std::int64_t{1} << 30)) {
    s.max_body_bytes = static_cast<std::uint64_t>(*v);
  }
  if (auto v = r.integer("read_timeout", 1, 3600)) s.read_timeout = Seconds(*v);
  if (auto v = r.integer("keep_alive_timeout", 1, 3600)) s.keep_alive_timeout = Seconds(*v);
  if (auto v = r.integer("write_timeout", 1, 3600)) s.write_timeout = Seconds(*v);
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

namespace {

ErrorMapping read_error_mapping(ObjectReader m) {
  ErrorMapping em;
  if (auto f = m.string("field", true)) {
    if (auto fp = read_path(*f, m.path_of("field"), m.diag())) {
      if (std::any_of(fp->begin(), fp->end(), [](const PathSegment& s) { return s.each; })) {
        m.diag()->error(m.path_of("field"), "错误码的路径不能带 []：它指向响应体中的一个值");
      } else {
        em.field = *fp;
      }
    }
  }
  if (m.raw("codes") == nullptr)
    m.string("codes", true);  // 登记为缺少的必填字段，在 finish() 中报告
  for_each_entry(m, "codes", [&](const std::string& code, const Json& v, const std::string& path) {
    if (!v.is_number_integer() || v.get<std::int64_t>() < 400 || v.get<std::int64_t>() > 599) {
      m.diag()->error(path, "应该是 400–599 之间的 HTTP 状态码，实际是 " + v.dump());
      return;
    }
    em.codes[code] = v.get<int>();
  });
  m.finish();
  return em;
}

}  // namespace

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
  if (auto v = r.string("ca_file")) {
    if (v->empty()) {
      r.diag()->error(r.path_of("ca_file"), "不能为空；要用系统证书库就不写这个字段");
    } else {
      p.ca_file = *v;
    }
  }
  if (auto m = r.object("error_mapping")) p.error_mapping = read_error_mapping(std::move(*m));
  r.unsupported("quota", "将在 v0.3 提供，任务 2.7");
  r.finish();
  return p;
}

namespace {

std::optional<std::string> env_either(const EnvLookup& getenv, const char* upper,
                                      const char* lower) {
  auto v = getenv(upper);
  if (!v || v->empty()) v = getenv(lower);
  return v;
}

// NO_PROXY="localhost, .corp.example.com,*.internal" → {"localhost", "corp.example.com",
// "internal"}
std::string_view trim(std::string_view s) {
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front())) != 0) s.remove_prefix(1);
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())) != 0) s.remove_suffix(1);
  return s;
}

// NO_PROXY 的一项规范化为主机名：去掉 "*." 和前导的点、端口（host:8080）、IPv6 的方括号
// （[::1]、[::1]:8080）、结尾的点（任务 1.4 独立审查）。"*" 保持不变
std::string_view no_proxy_host(std::string_view item) {
  if (item == "*") return item;
  if (item.starts_with("*.")) item.remove_prefix(2);
  while (item.starts_with('.')) item.remove_prefix(1);
  if (item.starts_with('[')) {
    const auto close = item.find(']');
    item = close == std::string_view::npos ? std::string_view{} : item.substr(1, close - 1);
  } else if (const auto colon = item.find(':');
             colon != std::string_view::npos &&
             item.find(':', colon + 1) == std::string_view::npos) {
    item = item.substr(0, colon);  // 恰好一个冒号：host:port；多个冒号是不带方括号的 IPv6 地址
  }
  while (item.ends_with('.')) item.remove_suffix(1);
  return item;
}

// NO_PROXY="localhost, .corp.example.com,*.internal" → {"localhost", "corp.example.com",
// "internal"}
std::vector<std::string> parse_no_proxy(std::string_view text) {
  std::vector<std::string> out;
  while (!text.empty()) {
    const auto comma = text.find(',');
    const std::string_view item = no_proxy_host(trim(text.substr(0, comma)));
    text = comma == std::string_view::npos ? std::string_view{} : text.substr(comma + 1);
    if (item.empty()) continue;
    std::string entry(item);
    std::transform(entry.begin(), entry.end(), entry.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    out.push_back(std::move(entry));
  }
  return out;
}

}  // namespace

namespace {

// proxy.https：默认代理。"env" 表示读取 HTTPS_PROXY（以及 NO_PROXY）
void read_default_proxy(ObjectReader& r, const EnvLookup& getenv, bool resolve_env,
                        ProxyConfig* proxy) {
  auto v = r.string("https");
  if (!v) return;
  std::string text = *v;
  // 错误信息中只说明来源，不回显代理地址：代理地址中可能带有用户名和密码（独立审查发现）
  std::string source = "代理地址";
  const bool from_env = text == "env";
  if (from_env) {
    // --skip-env 时不读取任何环境变量（与 LoadOptions::resolve_env 的约定一致）
    std::optional<std::string> env;
    if (resolve_env) {
      env = env_either(getenv, "HTTPS_PROXY", "https_proxy");
      if (auto no = env_either(getenv, "NO_PROXY", "no_proxy"))
        proxy->no_proxy = parse_no_proxy(*no);
    }
    text = env.value_or("");
    source = "环境变量 HTTPS_PROXY 中的代理地址";
  }
  if (text.empty()) return;
  auto p = parse_proxy_url(text);
  if (!p) {
    r.diag()->error(r.path_of("https"), source + "：" + p.error().message);
    return;
  }
  if (!from_env && !p->password.empty()) {
    r.diag()->warning(r.path_of("https"),
                      "代理密码写在了配置文件里；建议改为 \"env\"，通过 HTTPS_PROXY 环境变量提供");
  }
  proxy->https = std::move(*p);
}

// proxy.per_provider 的一项：代理地址或 "direct"
void read_provider_proxy(Diagnostics* diag, const std::string& name, const Json& v,
                         const std::string& path, ProxyConfig* proxy) {
  if (!v.is_string()) {
    diag->error(path, "应该是代理地址字符串或 \"direct\"，实际是" + type_name(v));
    return;
  }
  const auto text = v.get<std::string>();
  if (text == "direct") {
    proxy->per_provider[name] = std::nullopt;  // 这个上游不走代理
    return;
  }
  auto p = parse_proxy_url(text);
  if (!p) {
    diag->error(path, p.error().message);
    return;
  }
  if (!p->password.empty()) {
    diag->warning(path, "代理密码写在了配置文件里，注意不要把配置文件提交到代码仓库");
  }
  proxy->per_provider[name] = std::move(*p);
}

}  // namespace

ProxyConfig read_proxy(ObjectReader r, const EnvLookup& getenv, bool resolve_env) {
  ProxyConfig proxy;
  read_default_proxy(r, getenv, resolve_env, &proxy);
  for_each_entry(r, "per_provider",
                 [&](const std::string& name, const Json& v, const std::string& path) {
                   read_provider_proxy(r.diag(), name, v, path, &proxy);
                 });
  r.finish();
  return proxy;
}

UpstreamConfig read_upstream(ObjectReader r) {
  UpstreamConfig u;
  if (auto v = r.integer("idle_timeout", 1, 3600)) u.idle_timeout = Seconds(*v);
  r.finish();
  return u;
}

// 一条改写规则的路径，以及它在配置中的位置（报错用）
struct RuleAt {
  FieldPath path;
  std::string where;
};

bool strictly_under(const FieldPath& p, const FieldPath& prefix) {
  return p.size() > prefix.size() && std::equal(prefix.begin(), prefix.end(), p.begin());
}

// 规则之间的冲突（独立审查 1.6 #2、#4）：
// - 路径经过被改名的字段（来源或目标）、被设置的字段：按原名还是新名匹配、作用于原值还是设置的值，
//   都有歧义，要求写成一条规则
// - 两条改名指向同一个目标：输出重复的键；改名链：结果取决于执行顺序
void check_rule_conflicts(const TargetConfig& t, const std::vector<RuleAt>& rules,
                          const std::vector<RuleAt>& anchors, ObjectReader& r) {
  for (const auto& rule : rules) {
    for (const auto& a : anchors) {
      if (!strictly_under(rule.path, a.path)) continue;
      r.diag()->error(rule.where, "\"" + to_string(rule.path) +
                                      "\" 经过了被改名或者被设置的字段 \"" + to_string(a.path) +
                                      "\"，含义有歧义；请合并成一条规则（比如直接设置整个对象）");
      break;
    }
  }
  for (std::size_t i = 0; i < t.rename_fields.size(); ++i) {
    for (std::size_t j = 0; j < t.rename_fields.size(); ++j) {
      const auto& a = t.rename_fields[i];
      const auto& b = t.rename_fields[j];
      if (a.to == b.from) {
        r.diag()->error(
            r.path_of("rename_fields"),
            "\"" + to_string(b.from) +
                "\" 既是改名的目标又是改名的来源，结果取决于执行顺序；请合并成一次改名");
      }
      if (i < j && a.to == b.to) {
        r.diag()->error(r.path_of("rename_fields"), "\"" + to_string(a.from) + "\" 和 \"" +
                                                        to_string(b.from) + "\" 都改名为 \"" +
                                                        to_string(a.to) + "\"，结果会有重复的键");
      }
    }
  }
}

void read_rewrites(ObjectReader& r, TargetConfig* t) {
  std::vector<RuleAt> rules;  // 每条规则的路径（改名：来源，目标与它在同一个对象中）
  std::vector<RuleAt> anchors;  // 不能再往下写规则的字段：改名的来源和目标、设置的字段
  if (const Json* arr = r.array("drop_fields")) {
    for (std::size_t i = 0; i < arr->size(); ++i) {
      const auto path = index_path(r.path_of("drop_fields"), i);
      if (!(*arr)[i].is_string()) {
        r.diag()->error(path, "应该是字段路径字符串，比如 \"messages[].reasoning_content\"");
        continue;
      }
      if (auto fp = read_rewrite_path((*arr)[i].get<std::string>(), path, r.diag())) {
        t->drop_fields.push_back(*fp);
        rules.push_back({*fp, path});
      }
    }
  }
  for_each_entry(
      r, "rename_fields", [&](const std::string& from, const Json& v, const std::string& path) {
        auto f = read_rewrite_path(from, path, r.diag());
        if (!v.is_string()) {
          r.diag()->error(path, "应该是新的字段名（字符串），实际是" + type_name(v));
          return;
        }
        auto to = read_rewrite_path(v.get<std::string>(), path, r.diag());
        if (!f || !to) return;
        // 改名只在同一个父对象中进行：跨层的“改名”其实是移动，含义不清楚
        const bool same_parent =
            f->size() == to->size() && std::equal(f->begin(), f->end() - 1, to->begin());
        if (!same_parent) {
          r.diag()->error(path,
                          "改名的来源和目标必须在同一个对象中（比如 messages[].a → messages[].b）");
          return;
        }
        t->rename_fields.push_back({.from = *f, .to = *to});
        rules.push_back({*f, path});
        anchors.push_back({*f, path});
        anchors.push_back({*to, path});
      });
  for_each_entry(r, "set_fields",
                 [&](const std::string& key, const Json& v, const std::string& path) {
                   if (auto fp = read_rewrite_path(key, path, r.diag())) {
                     t->set_fields.push_back({.path = *fp, .json_value = v.dump()});
                     rules.push_back({*fp, path});
                     anchors.push_back({*fp, path});
                   }
                 });
  check_rule_conflicts(*t, rules, anchors, r);
}

TargetConfig read_target(ObjectReader r, const Timeouts& global) {
  TargetConfig t;
  if (auto v = r.string("provider", true)) t.provider = *v;
  if (auto v = r.string("model", true)) t.model = *v;
  read_rewrites(r, &t);
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
