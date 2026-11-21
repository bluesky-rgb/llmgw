#include "http_util.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdio>

namespace llmgw::server::detail {
namespace {

Result<ChatFields> parse(std::string body) { return parse_chat_fields(&body); }

TEST(ParseChatFields, reads_model_and_stream) {
  auto f = parse(R"({"model":"smart","stream":true,"messages":[]})");
  ASSERT_TRUE(f);
  EXPECT_EQ(f->model, "smart");
  EXPECT_TRUE(f->stream);
  auto d = parse(R"({"model":"fast"})");
  ASSERT_TRUE(d);
  EXPECT_FALSE(d->stream) << "没有 stream 时默认为 false";
  EXPECT_TRUE(parse(R"({"model":"m","stream":null})")) << "stream 为 null 等同于没有";
  // model 排在很大的字段之后、带转义字符的键
  auto late = parse(R"({"messages":[{"content":"x\"y"}],"stre\u0061m":true,"model":"smart"})");
  ASSERT_TRUE(late) << late.error().message;
  EXPECT_TRUE(late->stream);
}

TEST(ParseChatFields, explains_what_is_wrong) {
  struct Case {
    const char* body;
    const char* says;
  };
  for (const Case c : {Case{"", "不是合法的 JSON"}, Case{"{", "不是合法的 JSON"},
                       Case{"[]", "JSON 对象"}, Case{"{}", "缺少必填字段：model"},
                       Case{R"({"model":""})", "非空字符串"}, Case{R"({"model":1})", "非空字符串"},
                       Case{R"({"model":"m","stream":1})", "true 或 false"}}) {
    SCOPED_TRACE(c.body);
    auto r = parse(c.body);
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().code, ErrorCode::kInvalidRequest);
    EXPECT_NE(r.error().message.find(c.says), std::string::npos) << r.error().message;
  }
}

TEST(RequestId, keeps_valid_client_ids_and_generates_otherwise) {
  std::mt19937_64 rng(1);
  EXPECT_EQ(request_id("abc-123_x.y", rng), "abc-123_x.y");
  for (const char* bad : {"", "has space", "a/b", "\xe4\xb8\xad"}) {
    const auto id = request_id(bad, rng);
    EXPECT_EQ(id.size(), 16U) << bad;
    EXPECT_NE(id, bad);
  }
  EXPECT_EQ(request_id(std::string(65, 'a'), rng).size(), 16U) << "超过 64 个字符的要替换";
  EXPECT_NE(request_id("", rng), request_id("", rng));
}

TEST(ErrorBody, uses_openai_format_and_escapes_message) {
  const Error e{.code = ErrorCode::kRateLimited, .message = "say \"hi\"\n", .retry_after = {}};
  EXPECT_EQ(
      error_body(e),
      R"({"error":{"code":"rate_limited","message":"say \"hi\"\n","type":"rate_limit_error"}})");
  EXPECT_EQ(error_event(e).rfind("data: {\"error\":", 0), 0U);
  EXPECT_TRUE(error_event(e).ends_with("\n\n"));
}

TEST(Paths, strips_query_and_detects_hop_by_hop) {
  EXPECT_EQ(path_of("/v1/models?x=1"), "/v1/models");
  EXPECT_EQ(path_of("/v1/models"), "/v1/models");
  EXPECT_TRUE(is_hop_by_hop("Transfer-Encoding"));
  EXPECT_TRUE(is_hop_by_hop("connection"));
  EXPECT_FALSE(is_hop_by_hop("content-type"));
  EXPECT_FALSE(is_hop_by_hop("x-request-id"));
}

// 速度断言只在优化过、没有 sanitizer 的构建中生效：ASan/TSan 会让代码慢 5–10 倍
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
constexpr bool kMeasurable = false;
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer) || !defined(NDEBUG)
constexpr bool kMeasurable = false;
#else
constexpr bool kMeasurable = true;
#endif
#elif defined(NDEBUG)
constexpr bool kMeasurable = true;
#else
constexpr bool kMeasurable = false;
#endif

// S20：从请求体中取 model/stream 的开销。打印结果写入验证记录；断言一个下限速度，防止换成慢的实现
TEST(ParseChatFields, cost_on_typical_and_large_bodies) {
  auto body_of = [](std::size_t bytes) {
    std::string msg(bytes, 'a');
    return R"({"model":"smart","stream":true,"messages":[{"role":"user","content":")" + msg +
           R"("}]})";
  };
  for (const std::size_t size :
       {std::size_t{10} << 10, std::size_t{1} << 20, std::size_t{32} << 20}) {
    auto body = body_of(size);
    const int rounds = size > (std::size_t{1} << 20) ? 3 : 200;
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < rounds; ++i) ASSERT_TRUE(parse_chat_fields(&body));
    const auto us =
        std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start)
            .count() /
        rounds;
    std::printf("[S20] 请求体 %8zu KB：每次 %10.1f µs（约 %.0f MB/s）\n", size >> 10, us,
                static_cast<double>(size) / us);
    // 上界：至少 500 MB/s。nlohmann 只有约 140 MB/s，换回去这个测试会失败
    if (kMeasurable) {
      EXPECT_LT(us, static_cast<double>(size) / 500.0)
          << "比 500 MB/s 还慢：热路径上的解析会阻塞整个 Worker";
    }
  }
}

}  // namespace
}  // namespace llmgw::server::detail
