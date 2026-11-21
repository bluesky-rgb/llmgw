#include "llmgw/core/request_context.h"

#include <gtest/gtest.h>

#include <type_traits>
#include <utility>

namespace llmgw {
namespace {

// 单一写入者的编译期保证：Session 创建时确定的字段不能再被修改
static_assert(
    !std::is_assignable_v<decltype((std::declval<RequestContext&>().request)), RequestInfo>,
    "ctx.request 必须是只读的");
static_assert(!std::is_assignable_v<decltype((std::declval<RequestContext&>().config)),
                                    std::shared_ptr<const config::Snapshot>>,
              "ctx.config 必须是只读的");
// 一个请求只有一份上下文：不能复制，也不能移动（“移动”会退化成复制整个请求体，独立审查发现）
static_assert(!std::is_copy_constructible_v<RequestContext>);
static_assert(!std::is_copy_assignable_v<RequestContext>);
static_assert(!std::is_move_constructible_v<RequestContext>);

RequestContext make_ctx() {
  const auto now = SteadyClock::now();
  return {RequestInfo{.request_id = "req-1",
                      .model = "smart",
                      .stream = true,
                      .body = R"({"model":"smart"})",
                      .received_at = now,
                      .deadline = now + std::chrono::seconds(600)},
          nullptr};
}

TEST(RequestContext, starts_pending_with_no_attempts) {
  auto ctx = make_ctx();
  EXPECT_EQ(ctx.request.model, "smart");
  EXPECT_TRUE(ctx.request.stream);
  EXPECT_TRUE(ctx.auth.key_id.empty());
  EXPECT_TRUE(ctx.attempts.empty());
  EXPECT_FALSE(ctx.usage.reported);
  EXPECT_FALSE(ctx.ttft.has_value());
  EXPECT_EQ(ctx.outcome.kind, OutcomeKind::kPending);
  EXPECT_EQ(ctx.outcome.http_status, 0);
}

TEST(RequestContext, records_attempts_in_order) {
  auto ctx = make_ctx();
  ctx.attempts.push_back({.target = {"openai", "gpt"},
                          .result = AttemptResult::kStatus5xx,
                          .upstream_status = 503,
                          .elapsed = std::chrono::milliseconds(120)});
  ctx.attempts.push_back({.target = {"mimo", "mimo-pro"},
                          .result = AttemptResult::kOk,
                          .upstream_status = 200,
                          .elapsed = std::chrono::milliseconds(300)});
  ASSERT_EQ(ctx.attempts.size(), 2U);
  EXPECT_EQ(ctx.attempts[0].target.provider, "openai");
  EXPECT_EQ(ctx.attempts[1].result, AttemptResult::kOk);
}

TEST(RequestContext, factory_functions_still_work_without_moving) {
  // 返回 prvalue 时由 C++17 保证复制省略，不需要移动构造
  auto ctx = make_ctx();
  ctx.auth.key_id = "team-a";
  EXPECT_EQ(ctx.request.request_id, "req-1");
  EXPECT_EQ(ctx.auth.key_id, "team-a");
}

TEST(AttemptResult, labels_match_observability_doc) {
  // observability.md 3.2：result 标签取值 ok / timeout / 5xx / 429 / conn_error …
  EXPECT_EQ(label(AttemptResult::kOk), "ok");
  EXPECT_EQ(label(AttemptResult::kConnectError), "conn_error");
  EXPECT_EQ(label(AttemptResult::kTimeout), "timeout");
  EXPECT_EQ(label(AttemptResult::kStatus429), "429");
  EXPECT_EQ(label(AttemptResult::kStatus4xx), "4xx");
  EXPECT_EQ(label(AttemptResult::kStatus5xx), "5xx");
  EXPECT_EQ(label(AttemptResult::kBreakerOpen), "breaker_open");
  EXPECT_EQ(label(AttemptResult::kCancelled), "cancelled");
}

}  // namespace
}  // namespace llmgw
