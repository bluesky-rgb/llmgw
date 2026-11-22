// 重试规则表（任务 1.7 第 4 节）：design.md 6.1 的每一行 × retry_on_ambiguous。
// 集成测试（tests/integration/executor）再用真实的上游验证“第二个上游有没有收到请求”。
#include "classify.h"

#include <gtest/gtest.h>

namespace llmgw::executor::detail {
namespace {

using upstream::AttemptError;
using upstream::Stage;

constexpr Verdict next(AttemptResult r, bool ambiguous = false) {
  return {.result = r, .next = Next::kTryNext, .ambiguous = ambiguous};
}
constexpr Verdict ret(AttemptResult r, bool ambiguous = false) {
  return {.result = r, .next = Next::kReturn, .ambiguous = ambiguous};
}

AttemptError error(Stage stage, bool maybe_sent, bool timed_out) {
  return AttemptError{.stage = stage,
                      .maybe_sent = maybe_sent,
                      .timed_out = timed_out,
                      .reused_without_response = false,
                      .detail = "x"};
}

// 6.1 第 1 行：DNS、连接、TLS 失败 → 换下一个（与 retry_on_ambiguous 无关）
TEST(Classify, row1_not_sent_failures_try_next) {
  for (const bool retry : {false, true}) {
    for (const Stage s : {Stage::kResolve, Stage::kConnect, Stage::kProxy, Stage::kTls}) {
      SCOPED_TRACE(std::string(upstream::stage_name(s)) + (retry ? " retry" : ""));
      EXPECT_EQ(classify(error(s, false, false), retry), next(AttemptResult::kConnectError));
      EXPECT_EQ(classify(error(s, false, true), retry), next(AttemptResult::kTimeout));
    }
    // 补充：连接池排队超时（请求没有发出）
    EXPECT_EQ(classify(error(Stage::kQueue, false, true), retry), next(AttemptResult::kTimeout));
  }
}

// 6.1 第 2 行：复用的连接在发送阶段失败。upstream 内部已经换新连接重发一次（1.4 U4），
// 新连接上再失败时报告 kSend + maybe_sent = false
TEST(Classify, row2_send_failure_tries_next) {
  for (const bool retry : {false, true}) {
    EXPECT_EQ(classify(error(Stage::kSend, false, false), retry),
              next(AttemptResult::kConnectError));
    EXPECT_EQ(classify(error(Stage::kSend, false, true), retry), next(AttemptResult::kTimeout));
  }
}

// 6.1 第 3 行：429、502、503 → 换下一个
TEST(Classify, row3_429_502_503_try_next) {
  for (const bool retry : {false, true}) {
    EXPECT_EQ(classify(429, retry), next(AttemptResult::kStatus429));
    EXPECT_EQ(classify(502, retry), next(AttemptResult::kStatus5xx));
    EXPECT_EQ(classify(503, retry), next(AttemptResult::kStatus5xx));
  }
}

// 6.1 第 4 行：401、403、余额不足（402，含 error_mapping 映射的）→ 换下一个
TEST(Classify, row4_auth_and_balance_try_next) {
  for (const bool retry : {false, true}) {
    for (const int s : {401, 402, 403}) {
      SCOPED_TRACE(s);
      EXPECT_EQ(classify(s, retry), next(AttemptResult::kStatus4xx));
    }
  }
  EXPECT_TRUE(is_auth_error(401));
  EXPECT_TRUE(is_auth_error(403));
  EXPECT_FALSE(is_auth_error(402)) << "余额不足不是密钥失效";
}

// 6.1 第 5 行：已发出请求、未收到响应头时超时或断开 → 不确定：默认返回（504），可配置为重试
TEST(Classify, row5_maybe_sent_failures_are_ambiguous) {
  for (const Stage s : {Stage::kSend, Stage::kResponse, Stage::kBody}) {
    SCOPED_TRACE(upstream::stage_name(s));
    EXPECT_EQ(classify(error(s, true, true), false), ret(AttemptResult::kTimeout, true));
    EXPECT_EQ(classify(error(s, true, false), false), ret(AttemptResult::kConnectError, true));
    EXPECT_EQ(classify(error(s, true, true), true), next(AttemptResult::kTimeout, true));
    EXPECT_EQ(classify(error(s, true, false), true), next(AttemptResult::kConnectError, true));
  }
}

// 6.1 第 6 行：500、504（以及其他 5xx，偏离 3）→ 不确定：默认原样返回，可配置为重试
TEST(Classify, row6_500_504_and_other_5xx_are_ambiguous) {
  for (const int s : {500, 504, 501, 505, 520, 524, 599}) {
    SCOPED_TRACE(s);
    EXPECT_EQ(classify(s, false), ret(AttemptResult::kStatus5xx, true));
    EXPECT_EQ(classify(s, true), next(AttemptResult::kStatus5xx, true));
  }
}

// 6.1 第 7 行：其他 4xx → 原样返回
TEST(Classify, row7_other_4xx_are_returned) {
  for (const bool retry : {false, true}) {
    for (const int s : {400, 404, 405, 408, 409, 413, 422, 451, 499}) {
      SCOPED_TRACE(s);
      EXPECT_EQ(classify(s, retry), ret(AttemptResult::kStatus4xx));
    }
  }
}

// 2xx → 返回
TEST(Classify, success_is_returned) {
  for (const int s : {200, 201, 204, 299}) EXPECT_EQ(classify(s, false), ret(AttemptResult::kOk));
}

// 补充（偏离 4）：3xx → 换下一个
TEST(Classify, redirects_try_next) {
  for (const int s : {301, 302, 307, 308})
    EXPECT_EQ(classify(s, false), next(AttemptResult::kStatus5xx));
}

// 第 4 节规则 1：全部失败时，最后一次的哪些响应可以原样转发
TEST(Classify, responses_forwarded_when_exhausted) {
  for (const int s : {429, 502, 503}) EXPECT_TRUE(forward_when_exhausted(s));
  for (const int s : {401, 402, 403, 301, 500, 504}) EXPECT_FALSE(forward_when_exhausted(s));
}

}  // namespace
}  // namespace llmgw::executor::detail
