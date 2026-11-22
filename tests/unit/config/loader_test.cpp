// 配置加载与校验：设计文档 3.2 规则表的每一行至少一个用例（验证项 F1–F5、F8）。
#include "llmgw/config/loader.h"

#include <gtest/gtest.h>

#include <map>
#include <string>
#include <vector>

namespace llmgw::config {
namespace {

// 假的环境变量
LoadOptions with_env(std::map<std::string, std::string> vars) {
  LoadOptions o;
  o.getenv = [vars = std::move(vars)](const std::string& name) -> std::optional<std::string> {
    auto it = vars.find(name);
    return it == vars.end() ? std::nullopt : std::optional<std::string>(it->second);
  };
  return o;
}

LoadOptions skip_env() {
  LoadOptions o = with_env({});
  o.resolve_env = false;
  return o;
}

// 最小的有效配置；各个用例在它的基础上修改一处
std::string minimal(const std::string& extra_top = "", const std::string& provider_extra = "",
                    const std::string& target_extra = "") {
  return R"({ "providers": [ { "name": "openai", "base_url": "https://api.openai.com/v1",
                                "api_key_env": "OPENAI_API_KEY")" +
         provider_extra + R"( } ],
              "models": [ { "name": "smart", "targets": [ { "provider": "openai", "model": "gpt")" +
         target_extra + R"( } ] } ])" + extra_top + "}";
}

// 在报告中找指定路径的问题
const Issue* at(const LoadReport& r, const std::string& path) {
  for (const auto& i : r.issues) {
    if (i.path == path) return &i;
  }
  return nullptr;
}

std::string dump(const LoadReport& r) {
  std::string s;
  for (const auto& i : r.issues)
    s += (i.severity == Severity::kError ? "错误 " : "警告 ") + i.path + "：" + i.message + "\n";
  return s;
}

void expect_error(const std::string& json, const std::string& path,
                  const std::string& contains = "") {
  const auto r = parse(json, skip_env());
  EXPECT_FALSE(r.ok()) << dump(r);
  const Issue* i = at(r, path);
  ASSERT_NE(i, nullptr) << "没有在 " << path << " 报错。实际：\n" << dump(r);
  EXPECT_EQ(i->severity, Severity::kError);
  if (!contains.empty()) {
    EXPECT_NE(i->message.find(contains), std::string::npos) << i->message;
  }
}

void expect_warning(const std::string& json, const std::string& path,
                    const std::string& contains = "") {
  const auto r = parse(json, skip_env());
  EXPECT_TRUE(r.ok()) << "警告不应该让配置无效：\n" << dump(r);
  const Issue* i = at(r, path);
  ASSERT_NE(i, nullptr) << "没有在 " << path << " 给出警告。实际：\n" << dump(r);
  EXPECT_EQ(i->severity, Severity::kWarning);
  if (!contains.empty()) {
    EXPECT_NE(i->message.find(contains), std::string::npos) << i->message;
  }
}

// ---------- F1：完整配置 ----------

TEST(LoadExample, example_config_is_valid_and_fully_parsed) {
  auto r = load_file(
      LLMGW_SOURCE_DIR "/config/llmgw.example.json",
      with_env({{"OPENAI_API_KEY", "k1"}, {"MINIMAX_API_KEY", "k2"}, {"MIMO_API_KEY", "k3"}}));
  ASSERT_TRUE(r.ok()) << dump(r);
  EXPECT_TRUE(r.issues.empty()) << "示例配置不应该有警告：\n" << dump(r);
  const Snapshot& s = *r.snapshot;

  EXPECT_EQ(s.server.port, 8080);
  EXPECT_EQ(s.server.admin, (Endpoint{.host = "127.0.0.1", .port = 9090}));
  EXPECT_EQ(s.server.max_body_bytes, 32U << 20);
  ASSERT_EQ(s.providers.size(), 3U);
  EXPECT_EQ(s.find_provider("openai")->api_key, "k1");
  EXPECT_EQ(s.find_provider("mimo")->base_url.host, "token-plan-cn.xiaomimimo.com");
  EXPECT_EQ(s.upstream.idle_timeout, Seconds(60));
  const auto& em = s.find_provider("minimax")->error_mapping;
  ASSERT_TRUE(em.has_value());
  EXPECT_EQ(to_string(em->field), "base_resp.status_code");
  EXPECT_EQ(em->codes.at("1002"), 429);

  const ModelConfig* smart = s.find_model("smart");
  ASSERT_NE(smart, nullptr);
  ASSERT_EQ(smart->targets.size(), 3U);
  EXPECT_EQ(smart->targets[0].drop_fields.size(), 3U);
  EXPECT_EQ(to_string(smart->targets[0].drop_fields[0]), "messages[].reasoning_content");
  ASSERT_EQ(smart->targets[1].set_fields.size(), 1U);
  EXPECT_EQ(smart->targets[1].set_fields[0].json_value, R"("medium")");
  EXPECT_EQ(smart->targets[2].timeouts.first_byte, Seconds(120));  // 目标级覆盖
  EXPECT_EQ(smart->targets[2].timeouts.total, Seconds(600));       // 其余沿用全局值
  const auto& rename = s.find_model("fast")->targets[0].rename_fields;
  ASSERT_EQ(rename.size(), 1U);
  EXPECT_EQ(to_string(rename[0].to), "max_completion_tokens");
}

TEST(LoadDefaults, missing_sections_use_design_defaults) {
  auto r = parse(minimal(), with_env({{"OPENAI_API_KEY", "k"}}));
  ASSERT_TRUE(r.ok()) << dump(r);
  const Snapshot& s = *r.snapshot;
  EXPECT_EQ(s.server.host, "0.0.0.0");
  EXPECT_EQ(s.server.port, 8080);
  EXPECT_EQ(s.server.observability.port, 9100);
  EXPECT_EQ(s.server.workers, 0U);
  EXPECT_EQ(s.server.max_body_bytes, 32U << 20);
  EXPECT_EQ(s.server.read_timeout, Seconds(10));
  EXPECT_EQ(s.server.keep_alive_timeout, Seconds(60));
  EXPECT_EQ(s.timeouts, Timeouts{});
  EXPECT_EQ(s.timeouts.first_byte_non_stream, Seconds(300));
  EXPECT_FALSE(s.proxy.https.has_value());
  EXPECT_EQ(s.upstream, UpstreamConfig{});
  EXPECT_EQ(s.upstream.idle_timeout, Seconds(60));
  EXPECT_TRUE(s.find_provider("openai")->ca_file.empty());
  EXPECT_EQ(s.find_model("smart")->targets[0].timeouts, s.timeouts);
}

// ---------- F2：规则表 ----------

TEST(Rules, top_level_requires_providers_and_models) {
  expect_error(R"({"models": []})", "providers", "缺少必填字段");
  expect_error(R"({"providers": [], "models": []})", "providers", "至少需要一项");
  expect_error(R"([1])", "（顶层）", "对象");
}

TEST(Rules, not_yet_supported_sections_are_errors_not_ignored) {
  expect_error(minimal(R"(, "keys": [])"), "keys", "v0.3");
  expect_error(minimal(R"(, "pricing": [])"), "pricing", "尚未支持");
  expect_error(minimal(R"(, "accounting": {})"), "accounting", "尚未支持");
  expect_error(minimal(R"(, "retry": {})"), "retry", "尚未支持");
  expect_error(minimal("", R"(, "quota": {"rpm": 60})"), "providers[0].quota", "尚未支持");
}

TEST(Rules, server_ports_ranges_and_conflicts) {
  expect_error(minimal(R"(, "server": {"port": 70000})"), "server.port", "65535");
  expect_error(minimal(R"(, "server": {"port": 9100})"), "server.observability_listen", "相同");
  expect_error(minimal(R"(, "server": {"workers": 2000})"), "server.workers", "1024");
  expect_error(minimal(R"(, "server": {"max_connections": 0})"), "server.max_connections");
  expect_error(minimal(R"(, "server": {"max_body_bytes": 100})"), "server.max_body_bytes", "1024");
  expect_error(minimal(R"(, "server": {"read_timeout": 0})"), "server.read_timeout");
  expect_error(minimal(R"(, "server": {"keep_alive_timeout": 7200})"), "server.keep_alive_timeout",
               "3600");
  expect_error(minimal(R"(, "server": {"write_timeout": 0})"), "server.write_timeout");
  expect_error(minimal(R"(, "server": {"admin_listen": "127.0.0.1"})"), "server.admin_listen",
               "端口");
  expect_warning(minimal(R"(, "server": {"admin_listen": "0.0.0.0:9090"})"), "server.admin_listen",
                 "暴露");
}

TEST(Rules, timeouts_positive_integers_not_above_total) {
  expect_error(minimal(R"(, "timeouts": {"connect": 0})"), "timeouts.connect");
  expect_error(minimal(R"(, "timeouts": {"idle": 1.5})"), "timeouts.idle", "小数");
  expect_error(minimal(R"(, "timeouts": {"idle": 700})"), "timeouts.idle", "不能大于总超时");
  expect_error(minimal(R"(, "timeouts": {"total": 30})"), "timeouts.first_byte_non_stream",
               "不能大于总超时");
}

TEST(Rules, provider_fields) {
  expect_error(
      R"({"providers": [{"name": "a b", "base_url": "https://x", "api_key_env": "K"}], "models": []})",
      "providers[0].name", "指标标签");
  expect_error(minimal("", R"(, "max_connections": 0)"), "providers[0].max_connections");
  expect_error(R"({"providers": [{"name": "a", "base_url": "https://x?y=1", "api_key_env": "K"}],
                   "models": []})",
               "providers[0].base_url", "查询参数");
  expect_error(R"({"providers": [{"name": "a", "base_url": "https://x", "api_key_env": "sk-123"}],
                   "models": []})",
               "providers[0].api_key_env", "而不是密钥本身");
  expect_warning(
      R"({"providers": [{"name": "a", "base_url": "http://10.0.0.5/v1", "api_key_env": "K"}],
                     "models": [{"name": "m", "targets": [{"provider": "a", "model": "x"}]}]})",
      "providers[0].base_url", "明文");
  // 本机的 http 不警告（比如 mock 上游）
  auto local = parse(
      R"({"providers": [{"name": "a", "base_url": "http://127.0.0.1:18081/v1", "api_key_env": "K"}],
                         "models": [{"name": "m", "targets": [{"provider": "a", "model": "x"}]}]})",
      skip_env());
  EXPECT_TRUE(local.issues.empty()) << dump(local);
}

TEST(Rules, error_mapping) {
  expect_error(minimal("", R"(, "error_mapping": {"field": "a..b", "codes": {}})"),
               "providers[0].error_mapping.field");
  expect_error(minimal("", R"(, "error_mapping": {"field": "a", "codes": {"1002": 200}})"),
               "providers[0].error_mapping.codes.1002", "400–599");
  expect_error(minimal("", R"(, "error_mapping": {"field": "a"})"),
               "providers[0].error_mapping.codes", "缺少必填字段");
}

TEST(Rules, proxy) {
  auto env = with_env({{"HTTPS_PROXY", "http://127.0.0.1:7890"}, {"OPENAI_API_KEY", "k"}});
  auto r = parse(minimal(R"(, "proxy": {"https": "env"})"), env);
  ASSERT_TRUE(r.ok()) << dump(r);
  EXPECT_EQ(r.snapshot->proxy.https->url.port, 7890);

  auto none = parse(minimal(R"(, "proxy": {"https": "env"})"), skip_env());
  ASSERT_TRUE(none.ok());
  EXPECT_FALSE(none.snapshot->proxy.https.has_value()) << "环境变量为空表示不用代理";

  expect_error(minimal(R"(, "proxy": {"https": "socks5://x"})"), "proxy.https", "http://");
  expect_error(minimal(R"(, "proxy": {"per_provider": {"opneai": "http://p:1"}})"),
               "proxy.per_provider.opneai", "是不是 \"openai\"");
}

TEST(Rules, models_and_targets) {
  expect_error(minimal("", "", R"(, "drop_fields": ["a b"])"),
               "models[0].targets[0].drop_fields[0]");
  expect_error(minimal("", "", R"(, "drop_fields": "x")"), "models[0].targets[0].drop_fields",
               "数组");
  expect_error(minimal("", "", R"(, "rename_fields": {"a": "b", "b": "c"})"),
               "models[0].targets[0].rename_fields", "合并");
  expect_error(minimal("", "", R"(, "timeouts": {"total": 5})"),
               "models[0].targets[0].timeouts.total", "顶层");
  expect_error(minimal("", "", R"(, "timeouts": {"idle": 601})"),
               "models[0].targets[0].timeouts.idle");
  expect_error(R"({"providers": [{"name": "a", "base_url": "https://x", "api_key_env": "K"}],
                   "models": [{"name": "m", "targets": []}]})",
               "models[0].targets", "至少需要一个");
  expect_error(R"({"providers": [{"name": "a", "base_url": "https://x", "api_key_env": "K"}],
                   "models": [{"name": "my model", "targets": [{"provider": "a", "model": "x"}]}]})",
               "models[0].name", "指标标签");
}

TEST(Rules, set_fields_keep_json_value_text) {
  auto r = parse(
      minimal("", "", R"(, "set_fields": {"temperature": 0.2, "stop": ["\n"], "x": {"y": null}})"),
      skip_env());
  ASSERT_TRUE(r.ok()) << dump(r);
  const auto& set = r.snapshot->models[0].targets[0].set_fields;
  ASSERT_EQ(set.size(), 3U);
  EXPECT_EQ(set[0].json_value, "0.2");
  EXPECT_EQ(set[1].json_value, R"(["\n"])");
  EXPECT_EQ(set[2].json_value, R"({"y":null})");
}

TEST(Rules, cross_section_references_and_duplicates) {
  expect_error(
      minimal("", "", "")
          .replace(minimal().find(R"("provider": "openai")"), 20, R"("provider": "openia")"),
      "models[0].targets[0].provider", "是不是 \"openai\"");
  expect_error(R"({"providers": [{"name": "a", "base_url": "https://x", "api_key_env": "K"},
                                 {"name": "a", "base_url": "https://y", "api_key_env": "K"}],
                   "models": [{"name": "m", "targets": [{"provider": "a", "model": "x"}]}]})",
               "providers[1].name", "重名");
  expect_error(R"({"providers": [{"name": "a", "base_url": "https://x", "api_key_env": "K"}],
                   "models": [{"name": "m", "targets": [{"provider": "a", "model": "x"}]},
                              {"name": "m", "targets": [{"provider": "a", "model": "y"}]}]})",
               "models[1].name", "重名");
  expect_warning(R"({"providers": [{"name": "a", "base_url": "https://x", "api_key_env": "K"}],
                     "models": [{"name": "m", "targets": [{"provider": "a", "model": "x"},
                                                          {"provider": "a", "model": "x"}]}]})",
                 "models[0].targets[1]", "完全相同");
  expect_warning(R"({"providers": [{"name": "a", "base_url": "https://x", "api_key_env": "K"},
                                   {"name": "b", "base_url": "https://y", "api_key_env": "K"}],
                     "models": [{"name": "m", "targets": [{"provider": "a", "model": "x"}]}]})",
                 "providers[1]", "没有被任何模型使用");
}

TEST(Rules, api_keys_come_from_environment) {
  auto missing = parse(minimal(), with_env({}));
  ASSERT_FALSE(missing.ok());
  ASSERT_NE(at(missing, "providers[0].api_key_env"), nullptr);
  EXPECT_NE(at(missing, "providers[0].api_key_env")->message.find("--skip-env"), std::string::npos);

  auto empty = parse(minimal(), with_env({{"OPENAI_API_KEY", ""}}));
  EXPECT_FALSE(empty.ok()) << "空字符串等同于没有设置";

  auto skipped = parse(minimal(), skip_env());
  ASSERT_TRUE(skipped.ok());
  EXPECT_TRUE(skipped.snapshot->providers[0].api_key.empty());
}

// ---------- F3：一次列出所有问题 ----------

TEST(AllIssues, reports_every_problem_in_one_pass) {
  const auto r = parse(R"({
    "server": {"port": 0},
    "timeouts": {"idle": "60"},
    "providers": [{"name": "a", "base_url": "ftp://x", "api_key_env": "K"}],
    "models": [{"name": "m", "targets": [{"provider": "b", "model": "x"}]}],
    "pricing": []
  })",
                       skip_env());
  EXPECT_FALSE(r.ok());
  EXPECT_EQ(r.error_count(), 5U) << dump(r);
  for (const char* path : {"server.port", "timeouts.idle", "providers[0].base_url",
                           "models[0].targets[0].provider", "pricing"}) {
    EXPECT_NE(at(r, path), nullptr) << path << "\n" << dump(r);
  }
}

// ---------- F4：拼写建议 ----------

TEST(Suggestions, typos_get_the_right_suggestion) {
  expect_error(
      R"({"providers": [{"name": "a", "base_ulr": "https://x", "api_key_env": "K"}], "models": []})",
      "providers[0].base_ulr", "base_url");
  expect_error(minimal(R"(, "timeouts": {"conect": 5})"), "timeouts.conect", "是不是 \"connect\"");
  expect_error(minimal(R"(, "modles": [])"), "modles", "是不是 \"models\"");
  // 拼写错误与缺少的必填字段合并成一条
  auto r = parse(R"({"providers": [{"name": "a", "base_ulr": "https://x", "api_key_env": "K"}],
                     "models": [{"name": "m", "targets": [{"provider": "a", "model": "x"}]}]})",
                 skip_env());
  EXPECT_EQ(r.error_count(), 1U) << dump(r);
}

// ---------- F5：语法错误定位 ----------

TEST(Syntax, reports_line_and_column) {
  auto r =
      parse("{\n  \"providers\": [\n    { \"name\": \"a\" }\n    { \"name\": \"b\" }\n  ]\n}\n",
            skip_env());
  ASSERT_EQ(r.issues.size(), 1U);
  EXPECT_EQ(r.issues[0].path, "第 4 行第 5 列");
  EXPECT_NE(r.issues[0].message.find("JSON 语法错误"), std::string::npos);

  auto trailing = parse("{\"a\": 1,}", skip_env());
  ASSERT_EQ(trailing.issues.size(), 1U);
  EXPECT_EQ(trailing.issues[0].path, "第 1 行第 9 列");

  auto unterminated = parse("{\"a\": \"x", skip_env());
  ASSERT_EQ(unterminated.issues.size(), 1U);
  EXPECT_EQ(unterminated.issues[0].path.rfind("第 1 行", 0), 0U);
}

// 模糊测试发现：数字溢出时 nlohmann 抛出的是 out_of_range 而不是 parse_error，此前会让 llmgw check
// 崩溃
TEST(Syntax, number_overflow_is_an_error_not_a_crash) {
  auto r = parse(R"({"timeouts": {"connect": 1e999}})", skip_env());
  ASSERT_EQ(r.issues.size(), 1U) << dump(r);
  EXPECT_NE(r.issues[0].message.find("超出范围"), std::string::npos) << r.issues[0].message;
}

TEST(Syntax, skips_utf8_bom) {
  auto r = parse("\xEF\xBB\xBF" + minimal(), skip_env());
  EXPECT_TRUE(r.ok()) << dump(r);
}

TEST(Syntax, unreadable_file_is_reported_with_its_name) {
  auto r = load_file("/nonexistent/llmgw.json");
  ASSERT_EQ(r.issues.size(), 1U);
  EXPECT_EQ(r.issues[0].path, "/nonexistent/llmgw.json");
}

// ---------- F8：敏感信息 ----------

// 独立审查发现：带认证的代理地址（企业环境常见）的密码会出现在错误信息中，--skip-env 时也会读取。
// 1.4 起代理认证是合法的配置：不论地址有效与否（包括漏写 "@"
// 让密码被当成端口），消息中都不能出现密码
TEST(Secrets, proxy_credentials_never_appear_in_issues) {
  struct Case {
    const char* proxy;
    bool valid;
  };
  for (const Case c :
       {Case{"http://alice:S3cr3tP4ss@proxy.corp:3128", true},
        Case{"alice:S3cr3tP4ss@proxy.corp:3128", false}, Case{"http://alice:S3cr3tP4ss", false},
        Case{"http://alice:S3cr3tP4ss@proxy.corp:99999", false},
        Case{"https://alice:S3cr3tP4ss@proxy.corp", false},
        Case{"http://alice:S3cr3tP4ss@proxy.corp/x", false}}) {
    SCOPED_TRACE(c.proxy);
    auto r = parse(minimal(R"(, "proxy": {"https": "env"})"),
                   with_env({{"HTTPS_PROXY", c.proxy}, {"OPENAI_API_KEY", "k"}}));
    EXPECT_EQ(r.ok(), c.valid) << dump(r);
    for (const auto& i : r.issues)
      EXPECT_EQ(i.message.find("S3cr3tP4ss"), std::string::npos) << i.message;
    for (const std::string& extra :
         {std::string(R"(, "proxy": {"https": ")") + c.proxy + "\"}",
          std::string(R"(, "proxy": {"per_provider": {"openai": ")") + c.proxy + "\"}}"}) {
      auto literal = parse(minimal(extra), skip_env());
      EXPECT_EQ(literal.ok(), c.valid) << dump(literal);
      for (const auto& i : literal.issues)
        EXPECT_EQ(i.message.find("S3cr3tP4ss"), std::string::npos) << i.message;
    }
  }
}

// 1.4：代理认证、"direct"、NO_PROXY
TEST(Proxy, credentials_are_percent_decoded) {
  auto p = parse_proxy_url("http://us%40er:p%3Ass%25w@proxy.corp:3128/");
  ASSERT_TRUE(p.has_value()) << p.error().message;
  EXPECT_EQ(p->username, "us@er");
  EXPECT_EQ(p->password, "p:ss%w");
  EXPECT_EQ(p->url.host, "proxy.corp");
  EXPECT_EQ(p->url.port, 3128);
  auto plain = parse_proxy_url("http://127.0.0.1:7890");
  ASSERT_TRUE(plain.has_value());
  EXPECT_TRUE(plain->username.empty());
  auto no_pass = parse_proxy_url("http://bob@p");
  ASSERT_TRUE(no_pass.has_value());
  EXPECT_EQ(no_pass->username, "bob");
  EXPECT_EQ(no_pass->url.port, 80);
  EXPECT_FALSE(parse_proxy_url("http://a:%zz@p").has_value());
  EXPECT_FALSE(parse_proxy_url("http://a:%4@p").has_value());
  EXPECT_FALSE(parse_proxy_url("http://:pw@p").has_value());
}

TEST(Proxy, password_in_config_file_is_a_warning) {
  auto r = parse(minimal(R"(, "proxy": {"https": "http://a:pw@p:1"})"), skip_env());
  ASSERT_TRUE(r.ok()) << dump(r);
  const Issue* i = at(r, "proxy.https");
  ASSERT_NE(i, nullptr);
  EXPECT_EQ(i->severity, Severity::kWarning);
  EXPECT_EQ(r.snapshot->proxy.https->password, "pw");
}

TEST(Proxy, per_provider_direct_and_no_proxy_decide_the_route) {
  const std::string providers = R"({ "providers": [
      { "name": "openai", "base_url": "https://api.openai.com/v1", "api_key_env": "K" },
      { "name": "vllm",   "base_url": "http://gpu1.corp.example.com:8000/v1", "api_key_env": "K" },
      { "name": "local",  "base_url": "http://localhost:8000/v1", "api_key_env": "K" },
      { "name": "mimo",   "base_url": "https://x.xiaomimimo.com/v1", "api_key_env": "K" } ],
    "models": [ { "name": "m", "targets": [ { "provider": "openai", "model": "g" } ] } ],
    "proxy": { "https": "env", "per_provider": { "mimo": "direct" } } })";
  auto r = parse(providers, with_env({{"K", "k"},
                                      {"https_proxy", "http://u:p@127.0.0.1:7890"},
                                      {"no_proxy", " LOCALHOST, .corp.example.com ,, *.nothing"}}));
  ASSERT_TRUE(r.ok()) << dump(r);
  const Snapshot& s = *r.snapshot;
  EXPECT_EQ(s.proxy.no_proxy,
            (std::vector<std::string>{"localhost", "corp.example.com", "nothing"}));
  const ProxyUrl* p = s.proxy_for(*s.find_provider("openai"));
  ASSERT_NE(p, nullptr);
  EXPECT_EQ(p->url.port, 7890);
  EXPECT_EQ(p->username, "u");
  EXPECT_EQ(s.proxy_for(*s.find_provider("vllm")), nullptr) << "子域名匹配 NO_PROXY";
  EXPECT_EQ(s.proxy_for(*s.find_provider("local")), nullptr) << "NO_PROXY 不区分大小写";
  EXPECT_EQ(s.proxy_for(*s.find_provider("mimo")), nullptr) << "direct";

  auto all =
      parse(providers, with_env({{"K", "k"}, {"HTTPS_PROXY", "http://p:1"}, {"NO_PROXY", "*"}}));
  ASSERT_TRUE(all.ok());
  EXPECT_EQ(all.snapshot->proxy_for(*all.snapshot->find_provider("openai")), nullptr);
}

TEST(Proxy, no_proxy_entries_with_ports_brackets_and_dots) {
  auto r = parse(
      minimal(R"(, "proxy": {"https": "env"})"),
      with_env({{"OPENAI_API_KEY", "k"},
                {"HTTPS_PROXY", "http://p:1"},
                {"NO_PROXY", "gpu1:8000,[::1],[fe80::2]:443,::3,corp.example.com.,10.0.0.1:80"}}));
  ASSERT_TRUE(r.ok()) << dump(r);
  EXPECT_EQ(r.snapshot->proxy.no_proxy, (std::vector<std::string>{"gpu1", "::1", "fe80::2", "::3",
                                                                  "corp.example.com", "10.0.0.1"}));
  EXPECT_TRUE(no_proxy_matches("example.com", "api.example.com."));
  EXPECT_TRUE(no_proxy_matches("example.com.", "example.com."));
  EXPECT_TRUE(no_proxy_matches("example.com.", "api.example.com"));
}

TEST(Rules, ipv6_zone_id_is_rejected) {
  EXPECT_FALSE(parse_url("https://[fe80::1%25eth0]:443/v1").has_value());
  EXPECT_FALSE(parse_url("https://[fe80::1%eth0]/v1").has_value());
  EXPECT_TRUE(parse_url("https://[fe80::1]/v1").has_value());
}

TEST(Proxy, no_proxy_matching) {
  EXPECT_TRUE(no_proxy_matches("example.com", "example.com"));
  EXPECT_TRUE(no_proxy_matches("example.com", "api.EXAMPLE.com"));
  EXPECT_FALSE(no_proxy_matches("example.com", "badexample.com"));
  EXPECT_FALSE(no_proxy_matches("api.example.com", "example.com"));
  EXPECT_TRUE(no_proxy_matches("*", "anything"));
  EXPECT_FALSE(no_proxy_matches("", "x"));
  EXPECT_TRUE(no_proxy_matches("10.0.0.5", "10.0.0.5"));
}

TEST(Rules, upstream_section_and_ca_file) {
  auto r =
      parse(minimal(R"(, "upstream": {"idle_timeout": 30})", R"(, "ca_file": "/etc/corp-ca.pem")"),
            skip_env());
  ASSERT_TRUE(r.ok()) << dump(r);
  EXPECT_EQ(r.snapshot->upstream.idle_timeout, Seconds(30));
  EXPECT_EQ(r.snapshot->find_provider("openai")->ca_file, "/etc/corp-ca.pem");
  expect_error(minimal(R"(, "upstream": {"idle_timeout": 0})"), "upstream.idle_timeout");
  expect_error(minimal(R"(, "upstream": {"idle": 3})"), "upstream.idle");
  expect_error(minimal("", R"(, "ca_file": "")"), "providers[0].ca_file");
}

TEST(Secrets, skip_env_does_not_read_proxy_environment) {
  auto o = with_env({{"HTTPS_PROXY", "http://127.0.0.1:7890"}});
  o.resolve_env = false;
  auto r = parse(minimal(R"(, "proxy": {"https": "env"})"), o);
  ASSERT_TRUE(r.ok()) << dump(r);
  EXPECT_FALSE(r.snapshot->proxy.https.has_value()) << "--skip-env 时不应该读取环境变量";
}

TEST(Secrets, api_key_never_appears_in_issues) {
  // 即使配置有错误，消息中也不能出现密钥
  auto r = parse(minimal(R"(, "server": {"port": 0})"),
                 with_env({{"OPENAI_API_KEY", "sk-VERY-SECRET"}}));
  for (const auto& i : r.issues)
    EXPECT_EQ(i.message.find("sk-VERY-SECRET"), std::string::npos) << i.message;
}

}  // namespace
}  // namespace llmgw::config

// 任务 1.6 补充的校验：改写的路径
namespace llmgw::config {
namespace {

TEST(Rewrites, valid_rules_are_accepted) {
  auto r = parse(minimal("", "",
                         R"(, "drop_fields": ["messages[].reasoning_content", "thinking"],
                              "rename_fields": {"max_tokens": "max_completion_tokens", "messages[].a": "messages[].b"},
                              "set_fields": {"stream_options.include_usage": true, "reasoning_effort": "medium"})"),
                 skip_env());
  ASSERT_TRUE(r.ok()) << dump(r);
  const auto& t = r.snapshot->find_model("smart")->targets[0];
  EXPECT_EQ(t.drop_fields.size(), 2U);
  EXPECT_EQ(t.rename_fields.size(), 2U);
  EXPECT_EQ(t.set_fields.size(), 2U);
}

TEST(Rewrites, rename_must_stay_in_the_same_object) {
  expect_error(minimal("", "", R"(, "rename_fields": {"messages[].a": "b"})"),
               "models[0].targets[0].rename_fields.messages[].a");
  expect_error(minimal("", "", R"(, "rename_fields": {"a.b": "c.b"})"),
               "models[0].targets[0].rename_fields.a.b");
}

TEST(Rewrites, model_and_stream_are_managed_by_the_gateway) {
  expect_error(minimal("", "", R"(, "drop_fields": ["model"])"),
               "models[0].targets[0].drop_fields[0]");
  expect_error(minimal("", "", R"(, "set_fields": {"stream": false})"),
               "models[0].targets[0].set_fields.stream");
  expect_error(minimal("", "", R"(, "rename_fields": {"x": "model"})"),
               "models[0].targets[0].rename_fields.x");
  // 审查 1.6 #3：model.x 会把 model 替换成对象
  expect_error(minimal("", "", R"(, "set_fields": {"model.x": 1})"),
               "models[0].targets[0].set_fields.model.x");
  expect_error(minimal("", "", R"(, "drop_fields": ["stream.x"])"),
               "models[0].targets[0].drop_fields[0]");
  auto nested = parse(minimal("", "", R"(, "set_fields": {"metadata.model": "x"})"), skip_env());
  EXPECT_TRUE(nested.ok()) << "嵌套的 model 不受限制：" << dump(nested);
}

// 审查 1.6 #4：两条改名指向同一个目标，输出重复的键
TEST(Rewrites, two_renames_cannot_share_a_target) {
  expect_error(minimal("", "", R"(, "rename_fields": {"a": "c", "b": "c"})"),
               "models[0].targets[0].rename_fields");
}

// 审查 1.6 #2：规则的路径经过改名的字段或者设置的字段时，按原名还是新名、作用于原值还是设置的值，
// 都有歧义。加载时拒绝，要求写成一条规则
TEST(Rewrites, rules_under_renamed_or_set_fields_are_rejected) {
  expect_error(
      minimal(
          "", "",
          R"(, "rename_fields": {"thinking": "reasoning"}, "drop_fields": ["thinking.budget"])"),
      "models[0].targets[0].drop_fields[0]");
  expect_error(
      minimal(
          "", "",
          R"(, "rename_fields": {"thinking": "reasoning"}, "drop_fields": ["reasoning.budget"])"),
      "models[0].targets[0].drop_fields[0]");
  expect_error(minimal("", "", R"(, "set_fields": {"extra": {"a": 1}, "extra.b": 2})"),
               "models[0].targets[0].set_fields.extra.b");
  expect_error(
      minimal("", "", R"(, "set_fields": {"extra": {"a": 1}}, "drop_fields": ["extra.a"])"),
      "models[0].targets[0].drop_fields[0]");
  expect_error(
      minimal(
          "", "",
          R"(, "rename_fields": {"messages[].a": "messages[].b"}, "set_fields": {"messages[].a.x": 1})"),
      "models[0].targets[0].set_fields.messages[].a.x");
  // 同一个字段上的删除 + 设置（先删后设）、经过 messages[] 的多条规则：没有歧义
  auto ok = parse(
      minimal(
          "", "",
          R"(, "drop_fields": ["a", "messages[].x"], "set_fields": {"a": 1, "messages[].y": 2})"),
      skip_env());
  EXPECT_TRUE(ok.ok()) << dump(ok);
}

// 改写器的递归深度由规则的路径决定：限制路径的段数
TEST(Rewrites, path_depth_is_limited) {
  std::string deep = "a";
  for (int i = 0; i < 32; ++i) deep += ".a";
  expect_error(minimal("", "", R"(, "drop_fields": [")" + deep + R"("])"),
               "models[0].targets[0].drop_fields[0]", "32");
  std::string ok = "a";
  for (int i = 1; i < 32; ++i) ok += ".a";
  auto r = parse(minimal("", "", R"(, "drop_fields": [")" + ok + R"("])"), skip_env());
  EXPECT_TRUE(r.ok()) << dump(r);
}

TEST(Rewrites, last_segment_cannot_be_each) {
  expect_error(minimal("", "", R"(, "drop_fields": ["messages[]"])"),
               "models[0].targets[0].drop_fields[0]");
}

TEST(Rewrites, error_mapping_field_cannot_be_each) {
  expect_error(minimal("", R"(, "error_mapping": {"field": "errors[].code", "codes": {"1": 500}})"),
               "providers[0].error_mapping.field");
}

}  // namespace
}  // namespace llmgw::config
