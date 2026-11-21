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

// 审查 #12：网关按哪个 model 路由、后面的组件又按哪个处理，不能有歧义（解析器差异）
TEST(ParseChatFields, rejects_duplicate_keys_and_trailing_content) {
  for (const char* body : {R"({"model":"cheap","model":"expensive"})", R"({"model":"m","stream":true,"stream":false})",
                           R"({"model":"m"} trailing)", R"({"model":"m"}{"model":"n"})"}) {
    SCOPED_TRACE(body);
    auto r = parse(body);
    EXPECT_FALSE(r) << "应该拒绝";
  }
  EXPECT_TRUE(parse("{\"model\":\"m\"}  \n")) << "末尾的空白是合法的";
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

TEST(Security, safe_header_rejects_injection_and_bad_names) {
  EXPECT_TRUE(safe_header("content-type", "text/event-stream"));
  EXPECT_TRUE(safe_header("x-ratelimit-remaining-tokens", "1000"));
  EXPECT_TRUE(safe_header("x-a", "tab\tis fine"));
  EXPECT_TRUE(safe_header("x-a", "UTF-8 值 也可以"));
  EXPECT_FALSE(safe_header("x-a", "a\r\nSet-Cookie: x=1"));
  EXPECT_FALSE(safe_header("x-a", "a\nb"));
  EXPECT_FALSE(safe_header("x-a", "a\rb"));
  EXPECT_FALSE(safe_header("x-a", std::string("a\0b", 3)));
  EXPECT_FALSE(safe_header("x-a", "del\x7f"));
  EXPECT_FALSE(safe_header("", "v"));
  EXPECT_FALSE(safe_header("x bad", "v"));
  EXPECT_FALSE(safe_header("x:bad", "v"));
  EXPECT_FALSE(safe_header("x\r\nbad", "v"));
}

TEST(Security, only_plain_chunked_transfer_encoding_is_accepted) {
  EXPECT_TRUE(is_plain_chunked("chunked"));
  EXPECT_TRUE(is_plain_chunked(" Chunked\t"));
  EXPECT_FALSE(is_plain_chunked("gzip, chunked"));
  EXPECT_FALSE(is_plain_chunked("chunked, gzip"));
  EXPECT_FALSE(is_plain_chunked("chunked,chunked"));
  EXPECT_FALSE(is_plain_chunked("identity"));
  EXPECT_FALSE(is_plain_chunked(""));
}

TEST(Paths, accepts_absolute_form) {
  EXPECT_EQ(path_of("http://gw.local/v1/models?x=1"), "/v1/models");
  EXPECT_EQ(path_of("HTTPS://gw.local:8443/v1/chat/completions"), "/v1/chat/completions");
  EXPECT_EQ(path_of("http://gw.local"), "/");
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
  // 最坏情况：model 和 stream 排在巨大的 messages 之后，必须跳过整个数组才能找到（审查 #13：原来放在了最前面）
  auto body_of = [](std::size_t bytes) {
    std::string msg(bytes, 'a');
    return R"({"messages":[{"role":"user","content":")" + msg + R"("}],"model":"smart","stream":true})";
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
