// 任务 1.5 独立审查发现的问题（每个用例先复现问题、确认修复前失败，再保护修复）。
#include <gtest/gtest.h>

#include <random>

#include "inspect.h"
#include "llmgw/stream/sse_parser.h"
#include "stream_test_util.h"

namespace llmgw::stream {
namespace {

// #1（主要）：预读第一个 data 事件时，之前的注释和空行没有上限
TEST(StreamReview, prefetch_before_first_data_is_bounded) {
  auto ctx = make_ctx();
  std::vector<Step> steps(4096, Step{std::string(4096, '\n')});  // 16 MB 的空行，没有任何 data
  steps.push_back({"data: {\"base_resp\":{\"status_code\":1002}}\n\n"});
  auto bytes = std::make_shared<std::size_t>(0);
  const Drained d = run(
      open_only(sse_head(), std::move(steps), &ctx,
                Options{.error_mapping = &kMiniMaxMapping, .max_event_bytes = 64 * 1024}, bytes));
  EXPECT_LE(*bytes, std::size_t{1} << 20) << "预读超过上限后应该放弃检查，而不是继续读";
  EXPECT_FALSE(d.error) << "放弃检查不是错误";
  EXPECT_EQ(d.status, 200) << "没有读到 data 事件：不做映射";
}

// #2（主要）：结束时补空行让事件超过上限：最后一个事件被丢弃，却报告成功
TEST(StreamReview, completing_the_last_event_past_the_limit_is_an_error) {
  SseParser p(64);
  // 最后一个事件 64 字节（等于上限），补上空行后 65（超过）
  p.feed("data: 1\n\ndata: " + std::string(57, 'x') + "\n");
  while (p.next()) {
  }
  EXPECT_FALSE(p.finish()) << "最后一个事件被丢弃时不能报告成功";
}

// #3（次要）：超过上限时，之前已经完整的事件是否转发取决于切块方式
TEST(StreamReview, complete_events_before_an_oversized_one_are_forwarded_whatever_the_split) {
  const std::string good =
      "data: {\"choices\":[],\"usage\":{\"prompt_tokens\":1,\"completion_tokens\":2}}\n\n";
  const std::string bad = "data: " + std::string(5000, 'x');
  for (const bool one_read : {true, false}) {
    SCOPED_TRACE(one_read ? "一次读到" : "分两次读到");
    auto ctx = make_ctx();
    std::vector<Step> steps =
        one_read ? std::vector<Step>{{good + bad}} : std::vector<Step>{{good}, {bad}};
    const Drained d =
        run(drain(sse_head(), std::move(steps), &ctx, Options{.max_event_bytes = 4096}));
    ASSERT_TRUE(d.error);
    EXPECT_EQ(d.all(), good);
    EXPECT_TRUE(ctx.usage.reported);
  }
}

// #4（次要）：响应被映射成错误状态时，不应该写入 usage（executor 会丢弃这次尝试）
TEST(StreamReview, mapped_error_response_does_not_report_usage_before_it_is_read) {
  auto ctx = make_ctx();
  const std::string body =
      R"({"base_resp":{"status_code":1002},"usage":{"prompt_tokens":5,"completion_tokens":0}})";
  const Drained d =
      run(open_only(json_head(), {{body}}, &ctx, Options{.error_mapping = &kMiniMaxMapping}));
  EXPECT_EQ(d.status, 429);
  EXPECT_FALSE(ctx.usage.reported) << "还没有被读取的响应不能写入 usage";
}

// #5（次要）：Content-Type 只按前缀匹配；重复的 Content-Type
TEST(StreamReview, content_type_must_be_exactly_event_stream) {
  for (const auto& [ct, sse] :
       std::vector<std::pair<std::string, bool>>{{"text/event-stream", true},
                                                 {"TEXT/EVENT-STREAM;charset=utf-8", true},
                                                 {"\ttext/event-stream ; charset=utf-8", true},
                                                 {"text/event-stream-foo", false},
                                                 {"text/event-streams", false}}) {
    SCOPED_TRACE(ct);
    auto ctx = make_ctx();
    const Drained d = run(drain(ResponseHead{.status = 200, .headers = {{"Content-Type", ct}}},
                                {{"data: 1\n\nx"}}, &ctx));
    // SSE 模式下结尾的 "x" 是半个事件（错误）；原样转发时不是。只有 "x"
    // 不行：没有任何事件的响应体也会原样转发（#9）
    EXPECT_EQ(!d.error.has_value(), !sse);
  }
  auto ctx = make_ctx();
  for (const bool json_first : {true, false}) {  // 两种顺序：只看第一个时，有一种顺序测不出来
    std::vector<Header> h{{"Content-Type", "application/json"},
                          {"Content-Type", "text/event-stream"}};
    if (!json_first) std::swap(h[0], h[1]);
    const Drained dup =
        run(drain(ResponseHead{.status = 200, .headers = h}, {{"data: 1\n\nx"}}, &ctx));
    EXPECT_FALSE(dup.error) << "Content-Type 不一致：不按 SSE 解析";
  }
}

// #6（次要）：usage 中的 null 子字段让整个 usage 丢失；类型不对的 details 被接受
TEST(StreamReview, null_counters_are_zero_and_wrong_details_are_rejected) {
  auto a = detail::extract_usage(R"({"usage":{"prompt_tokens":3,"completion_tokens":null,
                                     "completion_tokens_details":{"reasoning_tokens":null}}})");
  ASSERT_TRUE(a.has_value()) << "null 的计数按 0（按缺失处理）";
  EXPECT_EQ(a->usage.prompt_tokens, 3);
  EXPECT_EQ(a->usage.completion_tokens, 0);
  EXPECT_FALSE(
      detail::extract_usage(R"({"usage":{"prompt_tokens":3,"completion_tokens_details":5}})")
          .has_value())
      << "details 既不是对象也不是 null：类型不对";
}

// #8（风险）：流以 "data: [DONE]" 结束、缺少换行：比直连上游的结果更差（追加了一个错误事件）
TEST(StreamReview, unterminated_done_or_comment_at_the_end_is_not_an_error) {
  for (const std::string tail : {"data: [DONE]", "data:[DONE]", ": bye"}) {
    SCOPED_TRACE(tail);
    auto ctx = make_ctx();
    const Drained d = run(drain(sse_head(), {{"data: 1\n\n" + tail}}, &ctx));
    EXPECT_FALSE(d.error) << "正常结束";
  }
  auto ctx = make_ctx();
  const Drained after_done = run(drain(sse_head(), {{"data: [DONE]\n\ndata: {\"x"}}, &ctx));
  EXPECT_FALSE(after_done.error) << "[DONE] 之后的半个事件：丢弃（与 WHATWG 规范相同）";
  const Drained done = run(drain(sse_head(), {{"data: 1\n\ndata: [DONE]"}}, &ctx));
  EXPECT_EQ(done.all(), "data: 1\n\ndata: [DONE]\n\n") << "补全后转发，客户端能收到 [DONE]";
}

// #9（风险，1.8 中对 MiniMax 验证）：200 + text/event-stream，但响应体是一个 JSON（没有任何 SSE
// 事件）
TEST(StreamReview, event_stream_with_a_plain_json_body) {
  const std::string body = R"({"base_resp":{"status_code":1002,"status_msg":"rate limit"}})";
  for (const std::string ending : {"", "\n"}) {
    SCOPED_TRACE(ending.empty() ? "没有换行" : "以换行结束");
    auto ctx = make_ctx();
    const Drained mapped =
        run(drain(sse_head(), {{body + ending}}, &ctx, Options{.error_mapping = &kMiniMaxMapping}));
    EXPECT_FALSE(mapped.error) << "原样转发，而不是报错丢掉错误信息";
    EXPECT_EQ(mapped.status, 429);
    EXPECT_TRUE(mapped.all().starts_with(body));
    const Drained plain = run(drain(sse_head(), {{body + ending}}, &ctx));
    EXPECT_FALSE(plain.error) << "没有配置映射时也原样转发";
    EXPECT_TRUE(plain.all().starts_with(body));
  }
}

// #11：ResponseStream 层面的切块不变性，与去掉 usage 块、预读、上限组合
TEST(StreamReview, stream_level_output_is_independent_of_the_split) {
  const std::string text =
      ": ping\n\n"
      "data: {\"choices\":[{\"delta\":{\"content\":\"a\"}}],\"usage\":null}\r\n\r\n"
      "data: {\"choices\":[],\"usage\":{\"prompt_tokens\":9,\"completion_tokens\":2}}\n\n"
      "data: {\"base_resp\":{\"status_code\":0}}\n\n"
      "data: [DONE]";
  std::mt19937 rng(7);
  for (const bool strip : {false, true}) {
    for (const bool mapping : {false, true}) {
      const Options opt{.strip_usage_chunk = strip,
                        .error_mapping = mapping ? &kMiniMaxMapping : nullptr};
      auto c0 = make_ctx();
      const Drained whole = run(drain(sse_head(), {{text}}, &c0, opt));
      for (int round = 0; round < 300; ++round) {
        std::vector<Step> steps;
        std::size_t pos = 0;
        while (pos < text.size()) {
          const std::size_t n = 1 + rng() % 9;
          steps.push_back({text.substr(pos, n)});
          pos += n;
        }
        auto c = make_ctx();
        const Drained d = run(drain(sse_head(), std::move(steps), &c, opt));
        ASSERT_EQ(d.all(), whole.all());
        ASSERT_EQ(d.status, whole.status);
        ASSERT_EQ(d.error.has_value(), whole.error.has_value());
        ASSERT_EQ(c.usage.reported, c0.usage.reported);
        for (const auto& chunk : d.chunks)
          ASSERT_TRUE(chunk.ends_with("\n\n") || chunk.ends_with("\r\n\r\n"));
      }
    }
  }
}

}  // namespace
}  // namespace llmgw::stream
