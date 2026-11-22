// open() 与两种 ResponseStream：S3（usage 与去掉 usage 块）、S4（结束与出错）、S6（业务错误码）、
// S7（非 SSE 响应）、S8（TTFT 与取消）。
#include "llmgw/stream/stream.h"

#include <gtest/gtest.h>

#include <boost/asio/as_tuple.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/experimental/awaitable_operators.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <chrono>
#include <deque>
#include <optional>

#include "stream_test_util.h"

namespace llmgw::stream {
namespace {

using namespace asio::experimental::awaitable_operators;  // NOLINT(google-build-using-namespace)

const std::string kOpenAi =
    "data: {\"choices\":[{\"delta\":{\"content\":\"a\"}}],\"usage\":null}\n\n"
    "data: {\"choices\":[],\"usage\":{\"prompt_tokens\":9,\"completion_tokens\":2}}\n\n"
    "data: [DONE]\n\n";

// ---------- 按完整事件切块 ----------

TEST(SseStream, chunks_are_whole_events_whatever_the_upstream_split) {
  // 上游把事件切得很碎：每一块都必须是完整的事件
  std::vector<Step> steps;
  for (std::size_t i = 0; i < kOpenAi.size(); i += 7) steps.push_back({kOpenAi.substr(i, 7)});
  auto ctx = make_ctx();
  const Drained d = run(drain(sse_head(), steps, &ctx));
  ASSERT_FALSE(d.error) << d.error->message;
  EXPECT_EQ(d.all(), kOpenAi) << "转发的字节与上游完全相同";
  for (const auto& c : d.chunks)
    EXPECT_TRUE(c.ends_with("\n\n")) << "一块的结尾必须是事件的结尾：" << c;
}

TEST(SseStream, several_events_in_one_read_become_one_chunk) {
  auto ctx = make_ctx();
  const Drained d = run(drain(sse_head(), {{kOpenAi}}, &ctx));
  ASSERT_EQ(d.chunks.size(), 1U);
}

// ---------- S3 ----------

TEST(SseStream, S3_usage_is_taken_from_the_last_non_null_report) {
  auto ctx = make_ctx();
  const Drained d = run(drain(sse_head(), {{kOpenAi}}, &ctx));
  ASSERT_FALSE(d.error);
  EXPECT_TRUE(ctx.usage.reported);
  EXPECT_EQ(ctx.usage.prompt_tokens, 9);
  EXPECT_EQ(ctx.usage.completion_tokens, 2);
  EXPECT_EQ(d.all(), kOpenAi) << "没有要求去掉时，usage 块原样转发";
}

TEST(SseStream, S3_cumulative_usage_in_every_chunk_takes_the_last) {
  // 有的上游在每一块里都带上累计的 usage：必须取最后一个（反向验证时发现原来的测试测不出这一点）
  auto ctx = make_ctx();
  (void)run(drain(sse_head(),
                  {{"data: "
                    "{\"choices\":[{\"delta\":{\"content\":\"a\"}}],\"usage\":{\"prompt_tokens\":9,"
                    "\"completion_tokens\":1}}\n\n"},
                   {"data: "
                    "{\"choices\":[{\"delta\":{\"content\":\"b\"}}],\"usage\":{\"prompt_tokens\":9,"
                    "\"completion_tokens\":2}}\n\n"},
                   {"data: {\"choices\":[{\"delta\":{}}],\"usage\":null}\n\n"}},
                  &ctx));
  EXPECT_EQ(ctx.usage.completion_tokens, 2)
      << "后面的 null 不覆盖已经拿到的 usage，较新的数字覆盖较旧的";
}

TEST(SseStream, S3_usage_chunk_is_stripped_only_when_asked) {
  std::vector<Step> steps;  // 每个事件单独到达：去掉 usage 块的那一次读取不能返回空块
  for (std::size_t p = 0, q; (q = kOpenAi.find("\n\n", p)) != std::string::npos; p = q + 2) {
    steps.push_back({kOpenAi.substr(p, q + 2 - p)});
  }
  auto ctx = make_ctx();
  const Drained d = run(drain(sse_head(), steps, &ctx, Options{.strip_usage_chunk = true}));
  ASSERT_FALSE(d.error);
  EXPECT_EQ(d.all().find("prompt_tokens"), std::string::npos) << "客户端没有要求 usage";
  EXPECT_NE(d.all().find("[DONE]"), std::string::npos);
  EXPECT_EQ(ctx.usage.prompt_tokens, 9) << "usage 照常提取";
}

TEST(SseStream, S3_no_usage_means_not_reported) {
  auto ctx = make_ctx();
  (void)run(
      drain(sse_head(), {{"data: {\"choices\":[{\"delta\":{\"content\":\"x\"}}]}\n\n"}}, &ctx));
  EXPECT_FALSE(ctx.usage.reported);
}

// ---------- S4 ----------

TEST(SseStream, S4_missing_final_blank_line_is_completed) {
  auto ctx = make_ctx();
  const Drained d = run(drain(sse_head(), {{"data: 1\n\ndata: [DONE]\n"}}, &ctx));
  ASSERT_FALSE(d.error);
  EXPECT_EQ(d.all(), "data: 1\n\ndata: [DONE]\n\n");
}

TEST(SseStream, S4_stream_ending_mid_line_is_an_error) {
  auto ctx = make_ctx();
  const Drained d = run(drain(sse_head(), {{"data: 1\n\ndata: {\"cho"}}, &ctx));
  ASSERT_TRUE(d.error);
  EXPECT_EQ(d.all(), "data: 1\n\n") << "完整的事件照常转发，半个事件不转发";
}

TEST(SseStream, S4_source_error_is_passed_through) {
  auto ctx = make_ctx();
  const Drained d =
      run(drain(sse_head(), {{"data: 1\n\n"}}, &ctx, {},
                Error{.code = ErrorCode::kUpstreamTimeout, .message = "断开", .retry_after = {}}));
  ASSERT_TRUE(d.error);
  EXPECT_EQ(d.error->code, ErrorCode::kUpstreamTimeout);
  EXPECT_EQ(d.all(), "data: 1\n\n");
}

TEST(SseStream, S5_oversized_event_is_an_error) {
  auto ctx = make_ctx();
  std::vector<Step> steps(20, Step{"data: " + std::string(1000, 'x') + "\n"});
  const Drained d = run(drain(sse_head(), steps, &ctx, Options{.max_event_bytes = 4096}));
  ASSERT_TRUE(d.error);
  EXPECT_EQ(d.error->code, ErrorCode::kUpstreamUnavailable);
}

// ---------- S6 ----------

const ErrorMapping kMiniMax{.path = {"base_resp", "status_code"},
                            .codes = {{"1002", 429}, {"1008", 402}}};

TEST(ErrorMappingStream, S6_first_sse_event_is_checked) {
  auto ctx = make_ctx();
  const std::string body =
      "data: {\"choices\":[],\"base_resp\":{\"status_code\":1002,\"status_msg\":\"rate "
      "limit\"}}\n\n";
  const Drained d =
      run(drain(sse_head(), {{": ping\n\n"}, {body}}, &ctx, Options{.error_mapping = &kMiniMax}));
  ASSERT_FALSE(d.error);
  EXPECT_EQ(d.status, 429);
  EXPECT_EQ(d.all(), ": ping\n\n" + body) << "预读的数据不丢失、不重复";
}

TEST(ErrorMappingStream, S6_non_stream_body_is_checked_and_kept) {
  auto ctx = make_ctx();
  const std::string body =
      R"({"id":"x","choices":[],"base_resp":{"status_code":1008,"status_msg":"余额不足"}})";
  const Drained d = run(drain(json_head(), {{body.substr(0, 10)}, {body.substr(10)}}, &ctx,
                              Options{.error_mapping = &kMiniMax}));
  EXPECT_EQ(d.status, 402);
  EXPECT_EQ(d.all(), body);
}

TEST(ErrorMappingStream, S6_success_and_non_2xx_are_untouched) {
  auto ctx = make_ctx();
  const Drained ok = run(drain(sse_head(), {{"data: {\"base_resp\":{\"status_code\":0}}\n\n"}},
                               &ctx, Options{.error_mapping = &kMiniMax}));
  EXPECT_EQ(ok.status, 200);
  const Drained err = run(drain(json_head(500), {{R"({"base_resp":{"status_code":1002}})"}}, &ctx,
                                Options{.error_mapping = &kMiniMax}));
  EXPECT_EQ(err.status, 500) << "上游已经给出了错误状态码：不改";
}

TEST(ErrorMappingStream, S6_source_error_during_prefetch_fails_open) {
  // 必须是 open() 本身失败：此时还没有向客户端发送任何东西，executor 可以换一个上游；
  // 如果 open() 成功、错误到 next() 才出现，客户端已经收到了
  // 200（反向验证时发现原来的测试分不出这两种情况）
  for (const bool sse : {true, false}) {
    SCOPED_TRACE(sse ? "SSE" : "非流式");
    auto ctx = make_ctx();
    const Drained d = run(open_only(
        sse ? sse_head() : json_head(), {{": ping\n\n"}}, &ctx, Options{.error_mapping = &kMiniMax},
        nullptr, Error{.code = ErrorCode::kUpstreamTimeout, .message = "x", .retry_after = {}}));
    ASSERT_TRUE(d.error) << "open() 应该失败";
    EXPECT_EQ(d.error->code, ErrorCode::kUpstreamTimeout);
  }
}

// ---------- S7 ----------

TEST(BodyStream, S7_json_for_a_stream_request_and_error_bodies_pass_through) {
  for (const int status : {200, 400, 429, 500}) {
    SCOPED_TRACE(status);
    auto ctx = make_ctx();
    const std::string body =
        status == 200
            ? R"({"choices":[{"message":{"content":"x"}}],"usage":{"prompt_tokens":3,"completion_tokens":4}})"
            : R"({"error":{"message":"bad","type":"invalid_request_error"}})";
    // 流式请求得到 JSON、出错时的 JSON：没有 SSE 的空行，不能按 SSE 解析（任务设计第 3 节要点 2）
    const Drained d = run(drain(json_head(status), {{body.substr(0, 5)}, {body.substr(5)}}, &ctx));
    ASSERT_FALSE(d.error);
    EXPECT_EQ(d.status, status);
    EXPECT_EQ(d.all(), body);
    EXPECT_EQ(ctx.usage.reported, status == 200);
  }
}

TEST(BodyStream, S7_sse_content_type_with_error_status_passes_through) {
  auto ctx = make_ctx();
  // 先有一个事件，再跟着非 SSE 的内容：按 SSE 解析会报错。只用 "<html>..." 测不出来，
  // 因为没有任何事件的响应体也会原样转发（反向验证时发现）
  const std::string body = "event: x\n\n<html>bad gateway";
  const Drained d = run(drain(sse_head(502), {{body}}, &ctx));
  ASSERT_FALSE(d.error) << "非 2xx 不按 SSE 解析";
  EXPECT_EQ(d.status, 502);
  EXPECT_EQ(d.all(), body);
}

TEST(BodyStream, S7_oversized_body_is_forwarded_without_usage) {
  auto ctx = make_ctx();
  const std::string big = R"({"pad":")" + std::string(5000, 'x') +
                          R"(","usage":{"prompt_tokens":1,"completion_tokens":1}})";
  const Drained d = run(drain(json_head(), {{big.substr(0, 3000)}, {big.substr(3000)}}, &ctx,
                              Options{.error_mapping = &kMiniMax, .max_inspect_bytes = 1024}));
  ASSERT_FALSE(d.error);
  EXPECT_EQ(d.all(), big);
  EXPECT_EQ(d.status, 200) << "没有完整读到响应体：不做映射（独立审查 #11）";
  EXPECT_FALSE(ctx.usage.reported) << "副本超过上限：放弃 usage，而不是解析半个 JSON";
}

// ---------- S8 ----------

TEST(SseStream, S8_ttft_is_the_first_data_event_not_a_heartbeat) {
  auto ctx = make_ctx();  // received_at = 现在
  const Drained d = run(drain(sse_head(), {{": ping\n\n"}, {"data: 1\n\n", 120ms}}, &ctx));
  ASSERT_FALSE(d.error);
  ASSERT_TRUE(ctx.ttft.has_value());
  EXPECT_GE(*ctx.ttft, 110ms) << "心跳不算首字";
  EXPECT_LT(*ctx.ttft, 1s);
}

TEST(SseStream, S8_cancellation_returns_promptly_and_keeps_what_was_read) {
  // 第一块带着累计的 usage（completion_tokens = 1）；第二块（completion_tokens = 2）要 10
  // 秒后才到， 在等它时取消：已经拿到的 usage
  // 保持原样，不会被写成半个结果；取消之后流可以安全地析构
  auto ctx = make_ctx();
  const auto t0 = std::chrono::steady_clock::now();
  const std::string first =
      "data: "
      "{\"choices\":[{\"delta\":{}}],\"usage\":{\"prompt_tokens\":5,\"completion_tokens\":1}}\n\n";
  const std::string second =
      "data: "
      "{\"choices\":[{\"delta\":{}}],\"usage\":{\"prompt_tokens\":5,\"completion_tokens\":2}}\n\n";
  const bool finished = run([&]() -> Task<bool> {
    // 先构造好参数：GCC 13 在 co_await 表达式中直接使用花括号初始化时会出现内部错误（ICE）
    std::vector<Step> steps{Step{first}, Step{second, 10s}};
    auto source = std::make_unique<ScriptSource>(std::move(steps));
    const Options opt;
    auto s = co_await open(sse_head(), std::move(source), &ctx, opt);
    auto c = co_await (*s)->next();
    EXPECT_TRUE(c.has_value() && c->has_value());
    asio::steady_timer t(co_await asio::this_coro::executor, 50ms);
    auto r = co_await ((*s)->next() || t.async_wait(asio::as_tuple(asio::use_awaitable)));
    co_return r.index() == 0;
  }());
  EXPECT_FALSE(finished);
  EXPECT_LT(std::chrono::steady_clock::now() - t0, 2s);
  EXPECT_TRUE(ctx.usage.reported);
  EXPECT_EQ(ctx.usage.completion_tokens, 1);
}

}  // namespace
}  // namespace llmgw::stream

namespace llmgw::stream {
namespace {

#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__) || !defined(NDEBUG)
constexpr bool kMeasurable = false;
#else
constexpr bool kMeasurable = true;
#endif

// S9：典型的 OpenAI 流（每个事件约 230 字节，都带 "usage": null，需要解析 JSON），按 16 KB 一次读取
TEST(SseStream, S9_per_event_cost) {
  const std::string ev =
      "data: "
      "{\"id\":\"chatcmpl-abc123\",\"object\":\"chat.completion.chunk\",\"created\":1732000000,"
      "\"model\":\"gpt-5.6-sol\",\"choices\":[{\"index\":0,\"delta\":{\"content\":"
      "\"你好，这是一个片段\"},"
      "\"finish_reason\":null}],\"usage\":null}\n\n";
  constexpr int kEvents = 100000;
  std::string all;
  for (int i = 0; i < kEvents; ++i) all += ev;
  std::vector<Step> steps;
  for (std::size_t i = 0; i < all.size(); i += 16384) steps.push_back({all.substr(i, 16384)});
  auto ctx = make_ctx();
  const auto t0 = std::chrono::steady_clock::now();
  const Drained d = run(drain(sse_head(), std::move(steps), &ctx));
  const double ns =
      std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count() /
      kEvents;
  ASSERT_FALSE(d.error);
  EXPECT_EQ(d.all().size(), all.size());
  std::printf("  [每个事件 %.0f ns（%zu 字节，含 usage 检查），每块平均 %.1f 个事件]\n", ns,
              ev.size(), static_cast<double>(kEvents) / static_cast<double>(d.chunks.size()));
  if (kMeasurable) {
    EXPECT_LT(ns, 2000) << "每个事件应该在 2 µs 以内";
  }
}

}  // namespace
}  // namespace llmgw::stream
