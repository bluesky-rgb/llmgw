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
  auto env = with_env({{"HTTPS_PROXY", "http://127.0.0.1:7890"}});
  env.resolve_env = false;
  auto r = parse(minimal(R"(, "proxy": {"https": "env"})"), env);
  ASSERT_TRUE(r.ok()) << dump(r);
  EXPECT_EQ(r.snapshot->proxy.https->port, 7890);

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

TEST(Secrets, api_key_never_appears_in_issues) {
  // 即使配置有错误，消息中也不能出现密钥
  auto r = parse(minimal(R"(, "server": {"port": 0})"),
                 with_env({{"OPENAI_API_KEY", "sk-VERY-SECRET"}}));
  for (const auto& i : r.issues)
    EXPECT_EQ(i.message.find("sk-VERY-SECRET"), std::string::npos) << i.message;
}

}  // namespace
}  // namespace llmgw::config
