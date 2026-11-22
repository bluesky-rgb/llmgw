// 配置的 keys 部分（任务 2.2 验证项 A3）。重点：配置中不能出现 key 本身，错误信息中也不能回显它。
#include <gtest/gtest.h>

#include <string>

#include "llmgw/config/loader.h"

namespace llmgw::config {
namespace {

const std::string kHash = "sha256:" + std::string(64, 'a');

LoadReport load(const std::string& keys) {
  const std::string json =
      R"({ "providers": [ { "name": "openai", "base_url": "https://api.openai.com/v1",
                            "api_key_env": "OPENAI_API_KEY" } ],
           "models": [ { "name": "smart", "targets": [ { "provider": "openai", "model": "gpt" } ] } ],
           "keys": )" +
      keys + "}";
  LoadOptions o;
  o.resolve_env = false;
  return parse(json, o);
}

std::string all_text(const LoadReport& r) {
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

TEST(Keys, valid_keys_are_loaded_and_hash_is_lowercased) {
  auto r = load(R"([{"id": "app-chatbot", "hash": ")" + kHash + R"("},
                    {"id": "batch.v2", "hash": "sha256:)" +
                std::string(64, 'B') + R"("}])");
  ASSERT_TRUE(r.ok()) << all_text(r);
  ASSERT_EQ(r.snapshot->keys.size(), 2U);
  EXPECT_EQ(r.snapshot->keys[0], (KeyConfig{.id = "app-chatbot", .hash = kHash}));
  EXPECT_EQ(r.snapshot->keys[1].hash, "sha256:" + std::string(64, 'b'));
}

TEST(Keys, plaintext_key_is_rejected_and_never_echoed) {
  const std::string secret = "sk-gw-THIS_IS_A_SECRET_THAT_MUST_NOT_BE_PRINTED_0123456789";
  // 写在 hash 字段里
  auto in_hash = load(R"([{"id": "a", "hash": ")" + secret + R"("}])");
  EXPECT_TRUE(has_error_at(in_hash, "keys[0].hash", "key 本身")) << all_text(in_hash);
  EXPECT_EQ(all_text(in_hash).find("THIS_IS_A_SECRET"), std::string::npos) << all_text(in_hash);
  // 写成 key 字段
  auto in_key = load(R"([{"id": "a", "key": ")" + secret + R"(", "hash": ")" + kHash + R"("}])");
  EXPECT_TRUE(has_error_at(in_key, "keys[0].key", "只能写它的哈希")) << all_text(in_key);
  EXPECT_EQ(all_text(in_key).find("THIS_IS_A_SECRET"), std::string::npos);
  // 格式不对的哈希（可能是别的什么密钥）：也不回显
  auto bad = load(R"([{"id": "a", "hash": "md5:THIS_IS_A_SECRET"}])");
  EXPECT_TRUE(has_error_at(bad, "keys[0].hash", "sha256")) << all_text(bad);
  EXPECT_EQ(all_text(bad).find("THIS_IS_A_SECRET"), std::string::npos);
  // 未知字段：只报告字段名
  auto unknown =
      load(R"([{"id": "a", "hash": ")" + kHash + R"(", "secret": ")" + secret + R"("}])");
  EXPECT_TRUE(has_error_at(unknown, "keys[0].secret")) << all_text(unknown);
  EXPECT_EQ(all_text(unknown).find("THIS_IS_A_SECRET"), std::string::npos);
}

TEST(Keys, format_errors) {
  EXPECT_TRUE(has_error_at(load(R"({"id": "a"})"), "keys", "数组"));
  EXPECT_TRUE(has_error_at(load(R"([{"hash": ")" + kHash + R"("}])"), "keys[0].id"));
  EXPECT_TRUE(has_error_at(load(R"([{"id": "a"}])"), "keys[0].hash"));
  EXPECT_TRUE(
      has_error_at(load(R"([{"id": "has space", "hash": ")" + kHash + R"("}])"), "keys[0].id"));
  EXPECT_TRUE(has_error_at(
      load(R"([{"id": ")" + std::string(65, 'x') + R"(", "hash": ")" + kHash + R"("}])"),
      "keys[0].id"));
  EXPECT_TRUE(has_error_at(load(R"([{"id": "a", "hash": "sha256:abc"}])"), "keys[0].hash"));
  EXPECT_TRUE(has_error_at(
      load(R"([{"id": "a", "hash": "sha256:)" + std::string(64, 'g') + R"("}])"), "keys[0].hash"));
}

TEST(Keys, duplicate_ids_and_hashes) {
  auto r = load(R"([{"id": "a", "hash": ")" + kHash + R"("},
                    {"id": "a", "hash": "sha256:)" +
                std::string(64, 'c') + R"("},
                    {"id": "b", "hash": ")" +
                kHash + R"("}])");
  EXPECT_TRUE(has_error_at(r, "keys[1].id", "重名")) << all_text(r);
  EXPECT_TRUE(has_error_at(r, "keys[2].hash", "同一个 key")) << all_text(r);
}

TEST(Keys, limits_and_budget_are_not_supported_yet) {
  EXPECT_TRUE(has_error_at(load(R"([{"id": "a", "hash": ")" + kHash + R"(", "limits": {}}])"),
                           "keys[0].limits", "2.3"));
  EXPECT_TRUE(has_error_at(load(R"([{"id": "a", "hash": ")" + kHash + R"(", "budget": {}}])"),
                           "keys[0].budget", "2.4"));
}

}  // namespace
}  // namespace llmgw::config
