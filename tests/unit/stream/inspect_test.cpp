// usage 提取与业务错误码（S3、S6）。
#include "inspect.h"

#include <gtest/gtest.h>

namespace llmgw::stream::detail {
namespace {

TEST(Usage, S3_openai_final_chunk) {
  auto u = extract_usage(
      R"({"id":"c","choices":[],"usage":{"prompt_tokens":12,"completion_tokens":34,"total_tokens":46,)"
      R"("completion_tokens_details":{"reasoning_tokens":5,"audio_tokens":0},"prompt_tokens_details":{"cached_tokens":2}}})");
  ASSERT_TRUE(u.has_value());
  EXPECT_EQ(u->usage.prompt_tokens, 12);
  EXPECT_EQ(u->usage.completion_tokens, 34);
  EXPECT_EQ(u->usage.reasoning_tokens, 5);
  EXPECT_TRUE(u->usage.reported);
  EXPECT_TRUE(u->usage_only);
}

TEST(Usage, S3_usage_order_and_choices_after_usage) {
  auto u = extract_usage(
      R"({"usage":{"completion_tokens":1,"prompt_tokens":2},"choices":[{"delta":{}}]})");
  ASSERT_TRUE(u.has_value());
  EXPECT_EQ(u->usage.prompt_tokens, 2);
  EXPECT_FALSE(u->usage_only) << "choices 不为空：这是带内容的块，不能去掉";
}

TEST(Usage, S3_absent_null_or_invalid_usage_is_ignored) {
  EXPECT_FALSE(extract_usage(R"({"choices":[],"usage":null})").has_value());
  EXPECT_FALSE(extract_usage(R"({"choices":[{"delta":{"content":"usage"}}]})").has_value());
  EXPECT_FALSE(extract_usage(R"({"usage":{"prompt_tokens":"12"}})").has_value()) << "类型不对";
  EXPECT_FALSE(extract_usage(R"({"usage":{"prompt_tokens":-1}})").has_value()) << "负数";
  EXPECT_FALSE(extract_usage(R"({"usage":{"prompt_tokens":1.5}})").has_value()) << "不是整数";
  EXPECT_FALSE(extract_usage(R"({"usage":{"prompt_tokens":1})").has_value()) << "JSON 不完整";
  EXPECT_FALSE(extract_usage("[DONE]").has_value());
  EXPECT_FALSE(extract_usage("").has_value());
  auto partial = extract_usage(R"({"usage":{"completion_tokens":3}})");
  ASSERT_TRUE(partial.has_value()) << "缺少的字段按 0";
  EXPECT_EQ(partial->usage.prompt_tokens, 0);
  EXPECT_FALSE(partial->usage_only) << "没有 choices 字段：不能确定是只有 usage 的块";
}

TEST(Usage, S3_null_details_are_fine) {
  auto u = extract_usage(
      R"({"usage":{"prompt_tokens":1,"completion_tokens":2,"completion_tokens_details":null}})");
  ASSERT_TRUE(u.has_value());
  EXPECT_EQ(u->usage.reasoning_tokens, 0);
}

TEST(ErrorCodes, S6_number_or_string_values) {
  const ErrorMapping m{.path = {"base_resp", "status_code"},
                       .codes = {{"1002", 429}, {"1004", 401}}};
  EXPECT_EQ(mapped_status(R"({"base_resp":{"status_code":1002,"status_msg":"rate limit"}})", m),
            429);
  EXPECT_EQ(mapped_status(R"({"base_resp":{"status_code":"1004"}})", m), 401);
  EXPECT_EQ(mapped_status(R"({"id":"x","choices":[],"base_resp":{"status_code":1002}})", m), 429)
      << "字段在后面也能找到";
  EXPECT_FALSE(mapped_status(R"({"base_resp":{"status_code":0}})", m).has_value())
      << "0 不在表中：成功";
  EXPECT_FALSE(mapped_status(R"({"choices":[]})", m).has_value());
  EXPECT_FALSE(mapped_status(R"({"base_resp":{"status_code":{"x":1}}})", m).has_value());
  EXPECT_FALSE(mapped_status("not json", m).has_value());
  EXPECT_FALSE(mapped_status(R"({"base_resp":{"status_code":1002.0}})", m).has_value())
      << "按文本比较";
}

TEST(ErrorCodes, S6_path_segments_are_escaped) {
  const ErrorMapping m{.path = {"a/b", "c~d"}, .codes = {{"1", 500}}};
  EXPECT_EQ(mapped_status(R"({"a/b":{"c~d":1}})", m), 500);
}

}  // namespace
}  // namespace llmgw::stream::detail
