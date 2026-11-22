// 任务 1.6 验证项 P6：attempt() 串联 1.4 的真实 upstream::Client、1.5 的 stream::open()。
// 测试上游检查收到的请求（改写后的请求体、请求头），测试读出客户端最终看到的内容。
#include <nlohmann/json.hpp>

#include "../upstream/harness.h"
#include "llmgw/config/loader.h"
#include "llmgw/core/request_context.h"
#include "llmgw/provider/provider.h"

namespace llmgw::test {
namespace {

using Json = nlohmann::json;

struct Drained {
  int status = 0;
  std::vector<Header> headers;
  std::string body;
  std::optional<Error> error;  ///< next() 返回的错误
};

Task<Drained> drain(ResponseStream* s) {
  Drained d{.status = s->head().status, .headers = s->head().headers};
  for (;;) {
    auto c = co_await s->next();
    if (!c) {
      d.error = c.error();
      break;
    }
    if (!*c) break;
    d.body += (*c)->data;
  }
  co_return d;
}

std::string header(const std::vector<Header>& hs, std::string_view name) {
  for (const auto& h : hs) {
    if (h.name == name) return h.value;
  }
  return {};
}

std::uint16_t closed_port() {
  asio::io_context ioc;
  asio::ip::tcp::acceptor a(ioc, asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
  return a.local_endpoint().port();
}

// 一次完整的尝试：prepare → attempt → 读完
struct Attempt {
  Env env;
  const config::Provider* p = nullptr;
  config::TargetConfig target;
  std::unique_ptr<RequestContext> ctx;

  explicit Attempt(config::Provider provider) {
    provider.api_key = "sk-SECRET-1";
    p = env.start(std::move(provider));
    target.provider = "up";
    target.model = "real-model";
  }
  Expected<Drained, upstream::AttemptError> run(std::string body, bool stream) {
    RequestInfo info;
    info.model = "smart";
    info.stream = stream;
    info.body = std::move(body);
    info.received_at = SteadyClock::now();
    auto prepared = provider::prepare(info, *p, target);
    if (!prepared) throw std::runtime_error(prepared.error().message);
    ctx = std::make_unique<RequestContext>(std::move(info), nullptr);
    auto task = [](Attempt* self,
                   provider::Prepared pr) -> Task<Expected<Drained, upstream::AttemptError>> {
      auto s = co_await provider::attempt(self->env.client.get(), self->p, std::move(pr),
                                          deadlines(), std::chrono::seconds(10), self->ctx.get());
      if (!s) co_return unexpected(s.error());
      co_return co_await drain(s->get());
    };
    return test::run(env.ioc, task(this, std::move(*prepared)));
  }
};

const char* const kUsage = R"({"choices":[],"usage":{"prompt_tokens":11,"completion_tokens":7}})";

std::vector<std::string> sse_events(bool with_usage) {
  std::vector<std::string> e{R"(data: {"choices":[{"delta":{"content":"he"}}]})"
                             "\n\n",
                             R"(data: {"choices":[{"delta":{"content":"llo"}}]})"
                             "\n\n"};
  if (with_usage) e.push_back(std::string("data: ") + kUsage + "\n\n");
  e.emplace_back("data: [DONE]\n\n");
  return e;
}

TEST(ProviderAttempt, P6_stream_rewrites_request_and_strips_injected_usage_chunk) {
  TestUpstream up([](const Received&) {
    Reply r = chunked(sse_events(true));
    // 额外的响应头：Set-Cookie 等不能转发，请求 ID 改名转发
    r.parts[0].bytes =
        "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nTransfer-Encoding: chunked\r\n"
        "Set-Cookie: s=1\r\nx-request-id: up-42\r\n\r\n";
    return r;
  });
  Attempt a(provider("http", "127.0.0.1", up.port()));
  a.target.drop_fields = {*config::parse_field_path("messages[].reasoning_content")};
  auto out = a.run(
      R"({"model":"smart","stream":true,"messages":[{"role":"user","content":"hi","reasoning_content":"x"}]})",
      true);
  ASSERT_TRUE(out.has_value()) << out.error().detail;
  EXPECT_FALSE(out->error.has_value());
  EXPECT_EQ(out->status, 200);
  EXPECT_EQ(out->body.find("usage"), std::string::npos)
      << "网关加上的 include_usage：usage 块不能转发给客户端";
  EXPECT_NE(out->body.find("llo"), std::string::npos);
  EXPECT_NE(out->body.find("[DONE]"), std::string::npos);
  EXPECT_EQ(a.ctx->usage.prompt_tokens, 11);
  EXPECT_EQ(a.ctx->usage.completion_tokens, 7);
  EXPECT_TRUE(a.ctx->ttft.has_value());
  EXPECT_EQ(header(out->headers, "Set-Cookie"), "");
  EXPECT_EQ(header(out->headers, "X-LLMGW-Upstream-Request-Id"), "up-42");

  const auto got = up.received();
  ASSERT_EQ(got.size(), 1U);
  EXPECT_EQ(got[0].target, "/chat/completions");
  EXPECT_EQ(got[0].header("Authorization"), "Bearer sk-SECRET-1");
  EXPECT_EQ(got[0].header("Accept"), "text/event-stream");
  const Json sent = Json::parse(got[0].body);
  EXPECT_EQ(sent["model"], "real-model");
  EXPECT_EQ(sent["stream_options"]["include_usage"], true);
  EXPECT_EQ(sent["messages"][0], (Json{{"role", "user"}, {"content", "hi"}}));
}

TEST(ProviderAttempt, P6_stream_keeps_usage_chunk_the_client_asked_for) {
  TestUpstream up([](const Received&) { return chunked(sse_events(true)); });
  Attempt a(provider("http", "127.0.0.1", up.port()));
  auto out =
      a.run(R"({"model":"smart","stream":true,"stream_options":{"include_usage":true}})", true);
  ASSERT_TRUE(out.has_value());
  EXPECT_NE(out->body.find(R"("prompt_tokens":11)"), std::string::npos);
  EXPECT_EQ(a.ctx->usage.prompt_tokens, 11);
}

TEST(ProviderAttempt, P6_non_stream_body_and_usage) {
  const std::string body =
      R"({"id":"x","choices":[{"message":{"content":"hi"}}],"usage":{"prompt_tokens":3,"completion_tokens":2}})";
  TestUpstream up([&](const Received&) { return ok(body); });
  Attempt a(provider("http", "127.0.0.1", up.port()));
  auto out = a.run(R"({"model":"smart","stream_options":{"include_usage":false}})", false);
  ASSERT_TRUE(out.has_value());
  EXPECT_EQ(out->body, body);
  EXPECT_EQ(header(out->headers, "Content-Type"), "application/json");
  EXPECT_EQ(a.ctx->usage.completion_tokens, 2);
  const auto got = up.received();
  ASSERT_EQ(got.size(), 1U);
  EXPECT_EQ(got[0].header("Accept"), "application/json");
  EXPECT_EQ(Json::parse(got[0].body)["stream_options"]["include_usage"], false)
      << "非流式请求不加 include_usage";
}

TEST(ProviderAttempt, P6_business_error_code_is_mapped) {
  TestUpstream up([](const Received&) {
    return ok(R"({"base_resp":{"status_code":1008,"status_msg":"余额不足"}})");
  });
  config::Provider p = provider("http", "127.0.0.1", up.port());
  p.error_mapping = config::ErrorMapping{
      .field = *config::parse_field_path("base_resp.status_code"), .codes = {{"1008", 402}}};
  Attempt a(std::move(p));
  auto out = a.run(R"({"model":"smart"})", false);
  ASSERT_TRUE(out.has_value());
  EXPECT_EQ(out->status, 402);
  EXPECT_NE(out->body.find("1008"), std::string::npos);
}

TEST(ProviderAttempt, P6_send_failure_is_returned_as_is) {
  Attempt a(provider("http", "127.0.0.1", closed_port()));
  auto out = a.run(R"({"model":"smart"})", false);
  ASSERT_FALSE(out.has_value());
  EXPECT_EQ(out.error().stage, upstream::Stage::kConnect);
  EXPECT_FALSE(out.error().maybe_sent);
  EXPECT_EQ(out.error().detail.find("SECRET"), std::string::npos);
}

TEST(ProviderAttempt, P6_prefetch_failure_is_maybe_sent) {
  // 配置了 error_mapping：open() 要读到第一个事件；上游在那之前断开
  TestUpstream up([](const Received&) {
    Reply r;
    r.parts = {Part{
        {},
        "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nTransfer-Encoding: chunked\r\n\r\n"
        "5\r\ndata:"}};
    r.close_after = true;
    return r;
  });
  config::Provider p = provider("http", "127.0.0.1", up.port());
  p.error_mapping = config::ErrorMapping{
      .field = *config::parse_field_path("base_resp.status_code"), .codes = {}};
  Attempt a(std::move(p));
  auto out = a.run(R"({"model":"smart","stream":true})", true);
  ASSERT_FALSE(out.has_value());
  EXPECT_EQ(out.error().stage, upstream::Stage::kBody);
  EXPECT_TRUE(out.error().maybe_sent) << "请求已经发出：executor 不能无条件重试";

  // 审查 1.6 #6：原样返回 upstream 的错误（timed_out、detail），不重新构造
  const Fetched direct = a.env.get(a.p);
  ASSERT_FALSE(direct.ok());
  EXPECT_EQ(out.error().stage, direct.error->stage);
  EXPECT_EQ(out.error().timed_out, direct.error->timed_out);
  EXPECT_EQ(out.error().detail, direct.error->detail);
}

TEST(ProviderAttempt, P6_disconnect_mid_stream_is_an_error_from_next) {
  TestUpstream up([](const Received&) {
    Reply r;
    r.parts = {Part{{},
                    "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nTransfer-Encoding: "
                    "chunked\r\n\r\n"},
               Part{{}, "30\r\ndata: {\"choices\":[{\"delta\":{\"content\":\"he\"}}]}\n\n\r\n"},
               Part{{}, "40\r\ndata: {\"choi"}};
    r.close_after = true;
    return r;
  });
  Attempt a(provider("http", "127.0.0.1", up.port()));
  auto out = a.run(R"({"model":"smart","stream":true})", true);
  ASSERT_TRUE(out.has_value()) << "响应头已经收到：attempt 成功，错误在读的过程中出现";
  ASSERT_TRUE(out->error.has_value());
  EXPECT_NE(out->body.find("\"he\""), std::string::npos) << "断开之前的完整事件已经转发";
  EXPECT_EQ(out->error->code, ErrorCode::kUpstreamTimeout) << "可能已发送时按 504 处理";
}

}  // namespace
}  // namespace llmgw::test
