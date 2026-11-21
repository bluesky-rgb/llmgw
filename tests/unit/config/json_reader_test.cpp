// json_reader 是内部组件；测试通过 src/config 的私有头文件访问（tests 目录可以看到模块内部）
#include "json_reader.h"

#include <gtest/gtest.h>

namespace llmgw::config::detail {
namespace {

TEST(EditDistance, counts_single_edits) {
  EXPECT_EQ(edit_distance("base_url", "base_url"), 0U);
  EXPECT_EQ(edit_distance("base_ulr", "base_url"), 2U);
  EXPECT_EQ(edit_distance("openia", "openai"), 2U);
  EXPECT_EQ(edit_distance("", "abc"), 3U);
}

TEST(DidYouMean, suggests_only_close_names) {
  const std::vector<std::string> fields{"connect", "first_byte", "idle", "total"};
  EXPECT_EQ(closest("conect", fields), "connect");
  EXPECT_EQ(closest("idel", fields), "idle");
  EXPECT_EQ(closest("timout", fields), "");  // 与任何字段都不接近：不乱猜
  EXPECT_EQ(did_you_mean("totl", fields), "（是不是 \"total\"？）");
}

TEST(ObjectReader, reports_unknown_fields_after_reading) {
  Diagnostics diag;
  auto json = Json::parse(R"({"port": 80, "hots": "x"})");
  ObjectReader r(json, "server", &diag);
  EXPECT_EQ(r.integer("port", 1, 65535), 80);
  r.string("host");
  r.finish();
  ASSERT_EQ(diag.issues().size(), 1U);
  EXPECT_EQ(diag.issues()[0].path, "server.hots");
  EXPECT_NE(diag.issues()[0].message.find("是不是 \"host\""), std::string::npos);
}

TEST(ObjectReader, merges_typo_with_missing_required_field) {
  Diagnostics diag;
  auto json = Json::parse(R"({"base_ulr": "https://x"})");
  ObjectReader r(json, "providers[0]", &diag);
  r.string("base_url", true);
  r.finish();
  ASSERT_EQ(diag.issues().size(), 1U) << "一个拼写错误只报一次";
  EXPECT_EQ(diag.issues()[0].path, "providers[0].base_ulr");
}

TEST(ObjectReader, rejects_wrong_types_and_ranges) {
  Diagnostics diag;
  auto json = Json::parse(R"({"a": "1", "b": 1.5, "c": -3, "d": 70000, "e": "yes"})");
  ObjectReader r(json, "x", &diag);
  EXPECT_FALSE(r.integer("a", 0, 10));
  EXPECT_FALSE(r.integer("b", 0, 10));
  EXPECT_FALSE(r.integer("c", 0, 10));
  EXPECT_FALSE(r.integer("d", 1, 65535));
  EXPECT_FALSE(r.boolean("e"));
  r.finish();
  ASSERT_EQ(diag.issues().size(), 5U);
  EXPECT_EQ(diag.issues()[1].path, "x.b");
  EXPECT_NE(diag.issues()[1].message.find("小数"), std::string::npos);
}

TEST(ObjectReader, unsupported_field_is_an_error_but_never_suggested) {
  Diagnostics diag;
  auto json = Json::parse(R"({"keys": [], "kyes": 1})");
  ObjectReader r(json, "", &diag);
  r.unsupported("keys", "v0.3");
  r.finish();
  ASSERT_EQ(diag.issues().size(), 2U);
  EXPECT_EQ(diag.issues()[0].path, "keys");
  EXPECT_EQ(diag.issues()[1].message.find("是不是"), std::string::npos)
      << "不应该建议写一个尚未支持的字段";
}

TEST(ObjectReader, non_object_is_reported_once) {
  Diagnostics diag;
  auto json = Json::parse(R"([1, 2])");
  ObjectReader r(json, "server", &diag);
  EXPECT_FALSE(r.valid());
  EXPECT_FALSE(r.string("host", true));  // 不再重复报告缺少字段
  r.finish();
  EXPECT_EQ(diag.issues().size(), 1U);
}

}  // namespace
}  // namespace llmgw::config::detail
