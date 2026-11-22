// 配置的 pipeline 部分（任务 2.1 验证项 M2）。插件名是否存在由 middleware 校验，见 middleware
// 的测试。
#include <gtest/gtest.h>

#include <nlohmann/json.hpp>
#include <string>

#include "llmgw/config/loader.h"

namespace llmgw::config {
namespace {

LoadReport load(const std::string& pipeline) {
  const std::string json =
      R"({ "providers": [ { "name": "openai", "base_url": "https://api.openai.com/v1",
                            "api_key_env": "OPENAI_API_KEY" } ],
           "models": [ { "name": "smart", "targets": [ { "provider": "openai", "model": "gpt" } ] } ])" +
      (pipeline.empty() ? std::string() : ", \"pipeline\": " + pipeline) + "}";
  LoadOptions o;
  o.resolve_env = false;
  return parse(json, o);
}

std::string issues(const LoadReport& r) {
  std::string s;
  for (const auto& i : r.issues) s += i.path + "：" + i.message + "\n";
  return s;
}

bool has_error_at(const LoadReport& r, const std::string& path, const std::string& contains = "") {
  for (const auto& i : r.issues) {
    if (i.severity == Severity::kError && i.path == path &&
        i.message.find(contains) != std::string::npos) {
      return true;
    }
  }
  return false;
}

TEST(Pipeline, absent_means_no_middleware) {
  auto r = load("");
  ASSERT_TRUE(r.ok()) << issues(r);
  EXPECT_TRUE(r.snapshot->pipeline.empty());
  auto empty = load("[]");
  ASSERT_TRUE(empty.ok()) << issues(empty);
  EXPECT_TRUE(empty.snapshot->pipeline.empty());
}

TEST(Pipeline, string_and_object_forms_keep_order_and_config) {
  auto r = load(R"(["auth", {"name": "ratelimit", "config": {"default_rpm": 600, "b": [1, "x"]}},
                   {"name": "budget"}])");
  ASSERT_TRUE(r.ok()) << issues(r);
  const auto& p = r.snapshot->pipeline;
  ASSERT_EQ(p.size(), 3U);
  EXPECT_EQ(p[0].name, "auth");
  EXPECT_EQ(p[0].config_json, "{}");
  EXPECT_EQ(p[0].path, "pipeline[0]");
  EXPECT_EQ(p[1].name, "ratelimit");
  EXPECT_EQ(p[1].path, "pipeline[1]");
  // 原样保存（重新解析后相等），由插件自己解析
  EXPECT_EQ(nlohmann::json::parse(p[1].config_json),
            nlohmann::json::parse(R"({"default_rpm": 600, "b": [1, "x"]})"));
  EXPECT_EQ(p[2].name, "budget");
  EXPECT_EQ(p[2].config_json, "{}");
}

TEST(Pipeline, malformed_entries_are_reported_with_paths) {
  EXPECT_TRUE(has_error_at(load(R"({"auth": true})"), "pipeline", "数组")) << issues(load(R"({})"));
  EXPECT_TRUE(has_error_at(load("[42]"), "pipeline[0]", "插件名"));
  EXPECT_TRUE(has_error_at(load(R"([""])"), "pipeline[0]", "不能为空"));
  // 对象形式的空名字也要报错，不能被悄悄丢掉（独立审查 2.1 #3：原来 check 会说“配置有效”）
  EXPECT_TRUE(has_error_at(load(R"([{"name": ""}])"), "pipeline[0].name", "不能为空"))
      << issues(load(R"([{"name": ""}])"));
  EXPECT_TRUE(has_error_at(load(R"([{"config": {}}])"), "pipeline[0].name"));
  EXPECT_TRUE(has_error_at(load(R"([{"name": "auth", "config": 1}])"), "pipeline[0].config"));
  // 未知字段：拼错的 config 给出建议
  auto typo = load(R"([{"name": "auth", "cofnig": {}}])");
  EXPECT_TRUE(has_error_at(typo, "pipeline[0].cofnig", "config")) << issues(typo);
}

TEST(Pipeline, duplicate_plugin_is_an_error) {
  auto r = load(R"(["auth", {"name": "auth"}])");
  EXPECT_FALSE(r.ok());
  EXPECT_TRUE(has_error_at(r, "pipeline[1]", "pipeline[0]")) << issues(r);
}

}  // namespace
}  // namespace llmgw::config
