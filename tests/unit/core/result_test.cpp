#include "llmgw/core/result.h"

#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <string>

namespace llmgw {
namespace {

Result<int> parse_positive(int v) {
  if (v <= 0) return fail(ErrorCode::kInvalidRequest, "must be positive");
  return v;
}

TEST(Result, holds_value_on_success) {
  auto r = parse_positive(3);
  ASSERT_TRUE(r);
  EXPECT_EQ(*r, 3);
}

TEST(Result, holds_error_on_failure) {
  auto r = parse_positive(-1);
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().code, ErrorCode::kInvalidRequest);
  EXPECT_EQ(r.error().message, "must be positive");
  EXPECT_FALSE(r.error().retry_after.has_value());
}

TEST(Result, fail_keeps_retry_after) {
  Result<void> r = fail(Error{.code = ErrorCode::kRateLimited,
                              .message = "slow down",
                              .retry_after = std::chrono::seconds(2)});
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().retry_after, std::chrono::seconds(2));
}

TEST(Result, and_then_stops_at_first_failure) {
  int calls = 0;
  auto doubled = [&](int v) -> Result<int> {
    ++calls;
    return v * 2;
  };
  EXPECT_EQ(parse_positive(4).and_then(doubled).value(), 8);
  EXPECT_FALSE(parse_positive(0).and_then(doubled));
  EXPECT_EQ(calls, 1);  // 失败之后不再执行
}

TEST(Result, works_with_move_only_values) {
  auto make = []() -> Result<std::unique_ptr<int>> { return std::make_unique<int>(7); };
  auto r = make();
  ASSERT_TRUE(r);
  std::unique_ptr<int> p = std::move(*r);
  EXPECT_EQ(*p, 7);
}

}  // namespace
}  // namespace llmgw
