// 错误码与 docs/client-guide.md 第 7 节的表格逐行核对。修改任一处都要同步另一处。
#include "llmgw/core/error.h"

#include <gtest/gtest.h>

#include <set>
#include <string>
#include <string_view>

namespace llmgw {
namespace {

struct Row {
  ErrorCode code;
  int status;
  std::string_view name;
  bool retry;
};

// 与 client-guide.md 第 7 节一致（500 internal_error 在任务 0.4 中补充）
constexpr Row kClientGuideTable[] = {
    {ErrorCode::kInvalidRequest, 400, "invalid_request", false},
    {ErrorCode::kInvalidApiKey, 401, "invalid_api_key", false},
    {ErrorCode::kBudgetExceeded, 402, "budget_exceeded", false},
    {ErrorCode::kModelNotFound, 404, "model_not_found", false},
    {ErrorCode::kPayloadTooLarge, 413, "payload_too_large", false},
    {ErrorCode::kRateLimited, 429, "rate_limited", true},
    {ErrorCode::kInternal, 500, "internal_error", false},
    {ErrorCode::kUpstreamUnavailable, 502, "upstream_unavailable", true},
    {ErrorCode::kOverloaded, 503, "overloaded", true},
    {ErrorCode::kUpstreamTimeout, 504, "upstream_timeout", false},
};

TEST(ErrorCode, matches_client_guide_table) {
  for (const auto& row : kClientGuideTable) {
    SCOPED_TRACE(std::string(row.name));
    EXPECT_EQ(http_status(row.code), row.status);
    EXPECT_EQ(code_name(row.code), row.name);
    EXPECT_EQ(should_retry(row.code), row.retry);
  }
}

TEST(ErrorCode, table_covers_every_code_exactly_once) {
  // 枚举值从 0 连续编号；表中的项数等于最后一个枚举值 + 1，且没有重复
  std::set<int> seen;
  for (const auto& row : kClientGuideTable) seen.insert(static_cast<int>(row.code));
  EXPECT_EQ(seen.size(), std::size(kClientGuideTable));
  EXPECT_EQ(*seen.rbegin(), static_cast<int>(ErrorCode::kUpstreamTimeout));
  EXPECT_EQ(*seen.begin(), 0);
}

TEST(ErrorCode, never_suggests_retry_after_request_may_have_been_billed) {
  // 504：请求已发到上游、可能已经计费；500：网关自身错误，可能也已经发出
  EXPECT_FALSE(should_retry(ErrorCode::kUpstreamTimeout));
  EXPECT_FALSE(should_retry(ErrorCode::kInternal));
}

TEST(ErrorCode, mappings_are_usable_at_compile_time) {
  static_assert(http_status(ErrorCode::kRateLimited) == 429);
  static_assert(code_name(ErrorCode::kOverloaded) == "overloaded");
  static_assert(should_retry(ErrorCode::kOverloaded));
}

}  // namespace
}  // namespace llmgw
