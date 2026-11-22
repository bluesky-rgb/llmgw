// 任务 1.7 验证项 X1–X16：Executor + 真实的 upstream::Client + 多个测试上游。
// “有没有重试”的唯一可信证据：后面的上游有没有收到请求（requests()）。
#include "llmgw/executor/executor.h"

#include <algorithm>
#include <boost/asio/as_tuple.hpp>
#include <boost/asio/experimental/awaitable_operators.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <nlohmann/json.hpp>

#include "../upstream/harness.h"
#include "llmgw/config/loader.h"
#include "llmgw/core/request_context.h"

namespace llmgw::test {
namespace {

using namespace asio::experimental::awaitable_operators;  // NOLINT(google-build-using-namespace)
using Json = nlohmann::json;
using tcp = asio::ip::tcp;

struct Drained {
  int status = 0;
  std::vector<Header> headers;
  std::string body;
  std::optional<Error> error;  ///< next() 返回的错误
  [[nodiscard]] std::string header(std::string_view name) const {
    for (const auto& h : headers) {
      if (h.name == name) return h.value;
    }
    return {};
  }
};

Task<Drained> drain(ResponseStream* s) {
  Drained d{.status = s->head().status, .headers = s->head().headers, .body = {}, .error = {}};
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

std::uint16_t closed_port() {
  asio::io_context ioc;
  asio::ip::tcp::acceptor a(ioc, asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
  return a.local_endpoint().port();
}

/// 一个上游状态码的响应（带 OpenAI 格式的错误体）
Reply status(int code, const std::string& extra_headers = "") {
  const std::string body =
      R"({"error":{"message":"from upstream )" + std::to_string(code) + R"("}})";
  return Reply{.parts = {Part{{},
                              "HTTP/1.1 " + std::to_string(code) +
                                  " X\r\nContent-Type: application/json\r\n" +
                                  "Content-Length: " + std::to_string(body.size()) + "\r\n" +
                                  extra_headers + "\r\n" + body}},
               .close_after = false,
               .rst = false,
               .hang = false};
}

const std::string kOkBody =
    R"({"choices":[{"message":{"content":"hi"}}],"usage":{"prompt_tokens":5,"completion_tokens":1}})";

/// 每个测试一份：配置（多个上游、一个模型）、io_context、Client、Executor
// 成员顺序：先销毁 executor、client，再销毁 ioc，最后是配置和指标（与 1.4 的 Env 相同）
struct XEnv {
  RecordingMetrics metrics;
  std::shared_ptr<config::Snapshot> config = std::make_shared<config::Snapshot>();
  asio::io_context ioc{1};
  std::unique_ptr<Client> client;
  std::unique_ptr<executor::Executor> executor;
  std::unique_ptr<RequestContext> ctx;

  XEnv() { config->models.push_back(config::ModelConfig{.name = "smart", .targets = {}}); }

  /// 加一个目标（上游名 = name）。在 start() 之前调用
  config::TargetConfig& add(const std::string& name, std::uint16_t port,
                            std::string scheme = "http", std::string host = "127.0.0.1",
                            std::uint32_t max_connections = 256) {
    config::Provider p = provider(scheme, host, port, max_connections);
    p.name = name;
    p.api_key = "sk-SECRET-" + name;
    config->providers.push_back(std::move(p));
    config::TargetConfig t;
    t.provider = name;
    t.model = name + "-model";
    config->models[0].targets.push_back(t);
    return config->models[0].targets.back();
  }
  void start(upstream::ClientOptions extra = {}) {
    auto tls = upstream::make_tls_contexts(*config);
    if (!tls) throw std::runtime_error(tls.error().message);
    extra.executor = ioc.get_executor();
    extra.config = config;
    extra.tls = *tls;
    extra.metrics = &metrics;
    client = std::make_unique<Client>(std::move(extra));
    executor = std::make_unique<executor::Executor>(
        executor::Options{.client = client.get(), .metrics = &metrics});
  }
  std::vector<const config::TargetConfig*> targets() const {
    std::vector<const config::TargetConfig*> out;
    for (const auto& t : config->models[0].targets) out.push_back(&t);
    return out;
  }
  void make_ctx(std::string body, bool stream, SteadyClock::duration total) {
    RequestInfo info;
    info.request_id = "req-1";
    info.model = "smart";
    info.stream = stream;
    info.body = std::move(body);
    info.received_at = SteadyClock::now();
    info.deadline = info.received_at + total;
    ctx = std::make_unique<RequestContext>(std::move(info), config);
  }
  Task<Expected<Drained, Error>> execute_and_drain() {
    const auto ts = targets();
    auto s = co_await executor->execute(ctx.get(), ts);
    if (!s) co_return unexpected(s.error());
    co_return co_await drain(s->get());
  }
  /// 执行一次并读完响应
  Expected<Drained, Error> run(std::string body = R"({"model":"smart"})", bool stream = false,
                               SteadyClock::duration total = std::chrono::seconds(30)) {
    make_ctx(std::move(body), stream, total);
    return test::run(ioc, execute_and_drain(), std::chrono::seconds(20));
  }
  std::vector<AttemptResult> results() const {
    std::vector<AttemptResult> r;
    for (const auto& a : ctx->attempts) r.push_back(a.result);
    return r;
  }
};

TestUpstream ok_upstream() {
  return TestUpstream(
      [](const Received&) { return ok(kOkBody, "Content-Type: application/json\r\n"); });
}

// ---------- X1：DNS、连接、TLS 失败 → 换下一个 ----------

TEST(Executor, X1_connect_dns_tls_failures_fail_over) {
  TestUpstream good = ok_upstream();
  TestUpstream bad_cert(UpstreamOptions{.tls = TlsMode::kWrongHost},
                        [](const Received&) { return ok("{}"); });
  struct Case {
    const char* name;
    std::function<void(XEnv*)> first;
  };
  for (const Case& c :
       {Case{"连接被拒绝", [](XEnv* e) { e->add("a", closed_port()); }},
        Case{"DNS 解析失败", [](XEnv* e) { e->add("a", 443, "http", "dns-fails.invalid"); }},
        Case{"证书主机名不匹配", [&](XEnv* e) {
               e->add("a", bad_cert.port(), "https", "localhost");
               e->config->providers.back().ca_file = test_pki().ca_file;
             }}}) {
    SCOPED_TRACE(c.name);
    XEnv env;
    c.first(&env);
    env.add("b", good.port());
    upstream::ClientOptions opt;
    opt.resolver = [](std::string host,
                      std::uint16_t port) -> Task<Result<std::vector<tcp::endpoint>>> {
      if (host.ends_with(".invalid")) co_return fail(ErrorCode::kUpstreamUnavailable, "NXDOMAIN");
      co_return std::vector<tcp::endpoint>{
          tcp::endpoint(asio::ip::make_address("127.0.0.1"), port)};
    };
    env.start(std::move(opt));
    const int before = good.requests();
    auto out = env.run();
    ASSERT_TRUE(out.has_value()) << out.error().message;
    EXPECT_EQ(out->status, 200);
    EXPECT_EQ(good.requests(), before + 1);
    EXPECT_EQ(env.results(), (std::vector{AttemptResult::kConnectError, AttemptResult::kOk}));
    EXPECT_EQ(out->header("X-LLMGW-Provider"), "b");
    EXPECT_EQ(out->header("X-LLMGW-Attempts"), "2");
  }
  EXPECT_EQ(bad_cert.requests(), 0);
}

// ---------- X2：连接池已满（排队超时）→ 换下一个 ----------

TEST(Executor, X2_pool_queue_timeout_fails_over) {
  TestUpstream hang([](const Received&) {
    return Reply{.parts = {}, .close_after = false, .rst = false, .hang = true};
  });
  TestUpstream good = ok_upstream();
  XEnv env;
  auto& a = env.add("a", hang.port(), "http", "127.0.0.1", 1);
  a.timeouts.connect = std::chrono::seconds(1);  // 排队计入连接时限
  env.add("b", good.port());
  env.start();
  // 先用一个挂起的请求占住 a 唯一的连接，同时执行
  auto task = [&]() -> Task<Expected<Drained, Error>> {
    auto occupy = [&]() -> Task<void> {
      (void)co_await fetch(env.client.get(), &env.config->providers[0], post(), deadlines(2s, 10s));
    };
    auto later = [&]() -> Task<Expected<Drained, Error>> {  // 先让 occupy 拿到连接
      asio::steady_timer t(env.ioc);
      t.expires_after(std::chrono::milliseconds(100));
      co_await t.async_wait(asio::use_awaitable);
      co_return co_await env.execute_and_drain();
    };
    auto r = co_await (later() || occupy());
    if (r.index() == 0) co_return std::get<0>(std::move(r));
    co_return unexpected(
        Error{.code = ErrorCode::kInternal, .message = "占用的请求先结束了", .retry_after = {}});
  };
  env.make_ctx(R"({"model":"smart"})", false, std::chrono::seconds(30));
  auto out = run(env.ioc, task(), std::chrono::seconds(20));
  ASSERT_TRUE(out.has_value()) << out.error().message;
  EXPECT_EQ(out->status, 200);
  EXPECT_EQ(env.results(), (std::vector{AttemptResult::kTimeout, AttemptResult::kOk}));
  EXPECT_EQ(hang.requests(), 1) << "只有占用连接的那一个请求";
}

// ---------- X3：429、502、503 → 换下一个 ----------

TEST(Executor, X3_429_502_503_fail_over) {
  for (const int code : {429, 502, 503}) {
    SCOPED_TRACE(code);
    TestUpstream first([code](const Received&) { return status(code); });
    TestUpstream good = ok_upstream();
    XEnv env;
    env.add("a", first.port());
    env.add("b", good.port());
    env.start();
    auto out = env.run();
    ASSERT_TRUE(out.has_value()) << out.error().message;
    EXPECT_EQ(out->status, 200);
    EXPECT_EQ(out->body, kOkBody);
    EXPECT_EQ(first.requests(), 1);
    EXPECT_EQ(good.requests(), 1);
    EXPECT_EQ(env.ctx->attempts[0].upstream_status, code);
    EXPECT_EQ(env.ctx->attempts[0].result,
              code == 429 ? AttemptResult::kStatus429 : AttemptResult::kStatus5xx);
  }
}

// ---------- X4：401、403、余额不足 → 换下一个；最后一个也失败时不转发 ----------

TEST(Executor, X4_auth_and_balance_errors_fail_over_and_are_never_forwarded) {
  for (const int code : {401, 403, 402}) {
    SCOPED_TRACE(code);
    TestUpstream first([code](const Received&) { return status(code); });
    TestUpstream good = ok_upstream();
    XEnv env;
    env.add("a", first.port());
    env.add("b", good.port());
    env.start();
    auto out = env.run();
    ASSERT_TRUE(out.has_value()) << out.error().message;
    EXPECT_EQ(out->status, 200);
    EXPECT_EQ(good.requests(), 1);
    EXPECT_EQ(env.metrics.counter(Counter::kUpstreamAuthErrors, {"a"}), code == 402 ? 0U : 1U);
  }
  // 余额不足由业务错误码映射而来（MiniMax 的 1008 → 402）
  TestUpstream minimax([](const Received&) {
    return ok(R"({"base_resp":{"status_code":1008,"status_msg":"insufficient balance"}})",
              "Content-Type: application/json\r\n");
  });
  TestUpstream good = ok_upstream();
  {
    XEnv env;
    env.add("a", minimax.port());
    env.config->providers[0].error_mapping = config::ErrorMapping{
        .field = *config::parse_field_path("base_resp.status_code"), .codes = {{"1008", 402}}};
    env.add("b", good.port());
    env.start();
    auto out = env.run();
    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(out->header("X-LLMGW-Provider"), "b");
    EXPECT_EQ(env.ctx->attempts[0].upstream_status, 402);
  }
  // 401 + 403：客户端得到网关的 502，看不到上游的响应体（那是网关自己的密钥问题）；
  // 错误信息中只有失败的分类，没有上游名（design.md 6.10）
  TestUpstream u1([](const Received&) { return status(401); });
  TestUpstream u2([](const Received&) { return status(403); });
  XEnv env;
  env.add("alpha-provider", u1.port());
  env.add("beta-provider", u2.port());
  env.start();
  auto out = env.run();
  ASSERT_FALSE(out.has_value());
  EXPECT_EQ(out.error().code, ErrorCode::kUpstreamUnavailable);
  EXPECT_EQ(out.error().message.find("from upstream"), std::string::npos);
  EXPECT_NE(out.error().message.find("401，403"), std::string::npos) << out.error().message;
  EXPECT_EQ(out.error().message.find("provider"), std::string::npos) << out.error().message;
  EXPECT_EQ(out.error().message.find("SECRET"), std::string::npos);
}

// ---------- X5：已发出请求后超时、断开 → 默认 504 不重试；配置后重试 ----------

TEST(Executor, X5_ambiguous_failures_are_not_retried_by_default) {
  TestUpstream hang([](const Received&) {
    return Reply{.parts = {}, .close_after = false, .rst = false, .hang = true};
  });
  TestUpstream reset([](const Received&) {
    return Reply{.parts = {}, .close_after = false, .rst = true, .hang = false};
  });
  struct Case {
    const char* name;
    std::uint16_t port;
    AttemptResult result;
  };
  for (const Case c : {Case{"收到请求后不响应（首字节超时）", hang.port(), AttemptResult::kTimeout},
                       Case{"收到请求后 RST", reset.port(), AttemptResult::kConnectError}}) {
    for (const bool retry : {false, true}) {
      SCOPED_TRACE(std::string(c.name) + (retry ? "，retry_on_ambiguous" : "，默认"));
      TestUpstream good = ok_upstream();
      XEnv env;
      env.config->retry.retry_on_ambiguous = retry;
      env.add("a", c.port).timeouts.first_byte_non_stream = std::chrono::seconds(1);
      env.add("b", good.port());
      env.start();
      auto out = env.run();
      if (!retry) {
        ASSERT_FALSE(out.has_value());
        EXPECT_EQ(out.error().code, ErrorCode::kUpstreamTimeout) << out.error().message;
        EXPECT_EQ(good.requests(), 0) << "可能已经计费：不能重试";
        EXPECT_EQ(env.results(), (std::vector{c.result}));
      } else {
        ASSERT_TRUE(out.has_value()) << out.error().message;
        EXPECT_EQ(good.requests(), 1);
        EXPECT_EQ(env.results(), (std::vector{c.result, AttemptResult::kOk}));
      }
    }
  }
}

// ---------- X6：500、504（及其他 5xx）→ 默认原样返回；配置后重试 ----------

TEST(Executor, X6_500_504_are_returned_as_is_by_default) {
  for (const int code : {500, 504, 501}) {
    for (const bool retry : {false, true}) {
      SCOPED_TRACE(std::to_string(code) + (retry ? " retry" : ""));
      TestUpstream first([code](const Received&) { return status(code); });
      TestUpstream good = ok_upstream();
      XEnv env;
      env.config->retry.retry_on_ambiguous = retry;
      env.add("a", first.port());
      env.add("b", good.port());
      env.start();
      auto out = env.run();
      ASSERT_TRUE(out.has_value());
      if (!retry) {
        EXPECT_EQ(out->status, code);
        EXPECT_NE(out->body.find("from upstream"), std::string::npos) << "原样返回上游的响应体";
        EXPECT_EQ(out->header("X-LLMGW-Provider"), "a");
        EXPECT_EQ(good.requests(), 0);
        EXPECT_EQ(out->header("x-should-retry"), "false") << "可能已经扣费：告诉 SDK 不要自动重试";
      } else {
        EXPECT_EQ(out->status, 200);
        EXPECT_EQ(good.requests(), 1);
      }
    }
  }
  // 最后一个目标没有下一个可换：即使配置了重试，也原样返回上游的 500
  TestUpstream u1([](const Received&) { return status(500); });
  TestUpstream u2([](const Received&) { return status(500); });
  XEnv env;
  env.config->retry.retry_on_ambiguous = true;
  env.add("a", u1.port());
  env.add("b", u2.port());
  env.start();
  auto out = env.run();
  ASSERT_TRUE(out.has_value());
  EXPECT_EQ(out->status, 500);
  EXPECT_EQ(out->header("X-LLMGW-Provider"), "b");
  EXPECT_EQ(out->header("X-LLMGW-Attempts"), "2");
}

// 独立审查 1.9 #3：x-should-retry 的三种情况
TEST(Executor, X6_should_retry_hint_on_returned_errors) {
  auto count = [](const Drained& d, std::string_view name) {
    return std::ranges::count_if(d.headers, [&](const Header& h) {
      return std::ranges::equal(h.name, name, [](char a, char b) {
        return std::tolower(static_cast<unsigned char>(a)) ==
               std::tolower(static_cast<unsigned char>(b));
      });
    });
  };
  {  // 之前有可能已扣费的尝试（重试了 500），最后原样返回 409：SDK 默认会重试 409，必须告诉它不要
    TestUpstream u1([](const Received&) { return status(500); });
    TestUpstream u2([](const Received&) { return status(409); });
    XEnv env;
    env.config->retry.retry_on_ambiguous = true;
    env.add("a", u1.port());
    env.add("b", u2.port());
    env.start();
    auto out = env.run();
    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(out->status, 409);
    EXPECT_EQ(out->header("x-should-retry"), "false");
  }
  {  // 上游自己说不要重试：转发
    TestUpstream u([](const Received&) { return status(400, "x-should-retry: false\r\n"); });
    XEnv env;
    env.add("a", u.port());
    env.start();
    auto out = env.run();
    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(out->header("x-should-retry"), "false");
  }
  {  // 上游说可以重试，但结果不确定（500）：以网关的 false 为准，只有一个头
    TestUpstream u([](const Received&) { return status(500, "x-should-retry: true\r\n"); });
    XEnv env;
    env.add("a", u.port());
    env.start();
    auto out = env.run();
    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(out->header("x-should-retry"), "false");
    EXPECT_EQ(count(*out, "x-should-retry"), 1);
  }
}

// ---------- X7：其他 4xx → 原样返回 ----------

TEST(Executor, X7_other_4xx_are_returned_as_is) {
  for (const int code : {400, 404, 422}) {
    SCOPED_TRACE(code);
    TestUpstream first([code](const Received&) { return status(code); });
    TestUpstream good = ok_upstream();
    XEnv env;
    env.add("a", first.port());
    env.add("b", good.port());
    env.start();
    auto out = env.run();
    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(out->status, code);
    EXPECT_NE(out->body.find("from upstream " + std::to_string(code)), std::string::npos);
    EXPECT_EQ(out->header("X-LLMGW-Provider"), "a");
    EXPECT_EQ(out->header("X-LLMGW-Attempts"), "1");
    EXPECT_EQ(good.requests(), 0);
    EXPECT_EQ(out->header("x-should-retry"), "") << "4xx 是确定的结果，不需要这个提示";
  }
}

// ---------- X8：流式输出开始以后中断 → 不切换 ----------

TEST(Executor, X8_mid_stream_failure_does_not_fail_over) {
  TestUpstream first([](const Received&) {
    Reply r;
    r.parts = {Part{{},
                    "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nTransfer-Encoding: "
                    "chunked\r\n\r\n"},
               Part{{}, "30\r\ndata: {\"choices\":[{\"delta\":{\"content\":\"he\"}}]}\n\n\r\n"},
               Part{{}, "40\r\ndata: {\"choi"}};
    r.close_after = true;
    return r;
  });
  TestUpstream good = ok_upstream();
  XEnv env;
  env.add("a", first.port());
  env.add("b", good.port());
  env.start();
  auto out = env.run(R"({"model":"smart","stream":true})", true);
  ASSERT_TRUE(out.has_value()) << "拿到了响应头：execute 成功，错误由 next() 返回";
  EXPECT_TRUE(out->error.has_value());
  EXPECT_NE(out->body.find("\"he\""), std::string::npos);
  EXPECT_EQ(good.requests(), 0) << "已经开始输出：不能换一个上游重新生成（ADR-003）";
}

// ---------- X9：全部失败 ----------

TEST(Executor, X9_all_targets_fail) {
  {  // 都连不上：502，信息中只有分类，没有上游名、内部地址、detail
    XEnv env;
    env.add("alpha-provider", closed_port());
    env.add("beta-provider", closed_port());
    env.start();
    auto out = env.run();
    ASSERT_FALSE(out.has_value());
    EXPECT_EQ(out.error().code, ErrorCode::kUpstreamUnavailable);
    EXPECT_NE(out.error().message.find("尝试了 2 次：连接失败，连接失败"), std::string::npos)
        << out.error().message;
    EXPECT_EQ(out.error().message.find("provider"), std::string::npos);
    EXPECT_EQ(out.error().message.find("127.0.0.1"), std::string::npos) << "不包含内部地址";
  }
  {  // 503 + 429（带 Retry-After）：429 和 Retry-After 交给客户端，但不转发上游的响应体
     // （独立审查 1.7 #6：上游的 429 / 代理生成的 502 页面中可能有组织 ID、内部主机名）
    TestUpstream u1([](const Received&) { return status(503); });
    TestUpstream u2([](const Received&) { return status(429, "Retry-After: 7\r\n"); });
    XEnv env;
    env.add("a", u1.port());
    env.add("b", u2.port());
    env.start();
    auto out = env.run();
    ASSERT_FALSE(out.has_value());
    EXPECT_EQ(out.error().code, ErrorCode::kRateLimited);
    EXPECT_EQ(out.error().retry_after, std::chrono::seconds(7));
    EXPECT_EQ(out.error().message.find("from upstream"), std::string::npos);
  }
  {  // 429 + 503（retry-after-ms）：最后一次是 503 → 502，Retry-After 向上取整到秒
    TestUpstream u1([](const Received&) { return status(429, "Retry-After: 9\r\n"); });
    TestUpstream u2([](const Received&) { return status(503, "retry-after-ms: 1500\r\n"); });
    XEnv env;
    env.add("a", u1.port());
    env.add("b", u2.port());
    env.start();
    auto out = env.run();
    ASSERT_FALSE(out.has_value());
    EXPECT_EQ(out.error().code, ErrorCode::kUpstreamUnavailable);
    EXPECT_EQ(out.error().retry_after, std::chrono::seconds(2));
  }
  {  // 不合理的 Retry-After 不转告客户端
    TestUpstream u1([](const Received&) { return status(429, "Retry-After: 99999999\r\n"); });
    TestUpstream u2([](const Received&) { return status(429, "Retry-After: soon\r\n"); });
    for (const std::uint16_t port : {u1.port(), u2.port()}) {
      XEnv env;
      env.add("a", port);
      env.start();
      auto out = env.run();
      ASSERT_FALSE(out.has_value());
      EXPECT_EQ(out.error().code, ErrorCode::kRateLimited);
      EXPECT_FALSE(out.error().retry_after.has_value());
    }
  }
  {  // 配置了重试：第一个超时（可能已计费）→ 504。最后一个是 429 也一样：不能让客户端按 Retry-After
     // 重来
    TestUpstream hang([](const Received&) {
      return Reply{.parts = {}, .close_after = false, .rst = false, .hang = true};
    });
    TestUpstream busy([](const Received&) { return status(429, "Retry-After: 1\r\n"); });
    for (const std::uint16_t second : {closed_port(), busy.port()}) {
      XEnv env;
      env.config->retry.retry_on_ambiguous = true;
      env.add("a", hang.port()).timeouts.first_byte_non_stream = std::chrono::seconds(1);
      env.add("b", second);
      env.start();
      auto out = env.run();
      ASSERT_FALSE(out.has_value());
      EXPECT_EQ(out.error().code, ErrorCode::kUpstreamTimeout) << out.error().message;
    }
  }
}

// ---------- X10：共享截止时间 ----------

TEST(Executor, X10_attempts_share_the_request_deadline) {
  TestUpstream hang([](const Received&) {
    return Reply{.parts = {}, .close_after = false, .rst = false, .hang = true};
  });
  TestUpstream good = ok_upstream();
  XEnv env;
  env.config->retry.retry_on_ambiguous = true;
  env.add("a", hang.port());  // 首字节超时 300 秒（非流式默认）
  env.add("b", good.port());
  env.start();
  const auto t0 = SteadyClock::now();
  auto out = env.run(R"({"model":"smart"})", false, std::chrono::seconds(1));
  const auto took = SteadyClock::now() - t0;
  EXPECT_LT(took, std::chrono::milliseconds(2500)) << "单次尝试的时限不能超过剩余的总时间";
  ASSERT_FALSE(out.has_value());
  EXPECT_EQ(out.error().code, ErrorCode::kUpstreamTimeout);
  EXPECT_EQ(good.requests(), 0) << "总时间用完：不再开始新的尝试";

  // 截止时间已经过去：一个请求都不发；没有不确定的尝试 → 502（独立审查 1.7 #3）
  XEnv late;
  late.add("a", good.port());
  late.start();
  auto none = late.run(R"({"model":"smart"})", false, -std::chrono::seconds(1));
  ASSERT_FALSE(none.has_value());
  EXPECT_EQ(none.error().code, ErrorCode::kUpstreamUnavailable) << none.error().message;
  EXPECT_EQ(good.requests(), 0);
  EXPECT_TRUE(late.ctx->attempts.empty());
}

// 连接阶段（包括排队）的时限也不超过剩余的总时间（独立审查 1.7 #4）
TEST(Executor, X10_connect_deadline_is_capped_by_the_request_deadline) {
  TestUpstream hang([](const Received&) {
    return Reply{.parts = {}, .close_after = false, .rst = false, .hang = true};
  });
  TestUpstream good = ok_upstream();
  XEnv env;
  env.add("a", hang.port(), "http", "127.0.0.1", 1).timeouts.connect = std::chrono::seconds(60);
  env.add("b", good.port());
  env.start();
  env.make_ctx(R"({"model":"smart"})", false, std::chrono::seconds(1));
  const auto t0 = SteadyClock::now();
  auto task = [&]() -> Task<Expected<Drained, Error>> {
    auto occupy = [&]() -> Task<void> {  // 占住 a 唯一的连接，让 execute 排队
      (void)co_await fetch(env.client.get(), &env.config->providers[0], post(), deadlines(2s, 10s));
    };
    auto later = [&]() -> Task<Expected<Drained, Error>> {
      asio::steady_timer t(env.ioc);
      t.expires_after(std::chrono::milliseconds(100));
      co_await t.async_wait(asio::use_awaitable);
      co_return co_await env.execute_and_drain();
    };
    auto r = co_await (later() || occupy());
    if (r.index() == 0) co_return std::get<0>(std::move(r));
    co_return unexpected(
        Error{.code = ErrorCode::kInternal, .message = "占用的请求先结束了", .retry_after = {}});
  };
  auto out = run(env.ioc, task(), std::chrono::seconds(20));
  EXPECT_LT(SteadyClock::now() - t0, std::chrono::milliseconds(2500)) << "排队等了目标的 60 秒";
  ASSERT_FALSE(out.has_value());
  EXPECT_EQ(env.ctx->attempts.at(0).result, AttemptResult::kTimeout);
  EXPECT_EQ(good.requests(), 0) << "时间用完：不再尝试下一个";
}

// 6.1 第 2 行：复用的连接在发送阶段就失败（上游在读请求体之前关闭）。upstream 内部换新连接重发，
// 对 executor 来说是同一个目标上的一次成功；新连接上也失败时（请求没有发出）换下一个目标
TEST(Executor, X1_send_failures_before_the_request_arrives) {
  const std::string big = R"({"model":"smart","pad":")" + std::string(16U << 20, 'x') + R"("})";
  {
    TestUpstream up(
        UpstreamOptions{
            .rst_before_body = [](const Received& r) { return r.connection == 1 && r.index == 2; }},
        [](const Received&) { return ok(kOkBody, "Content-Type: application/json\r\n"); });
    TestUpstream good = ok_upstream();
    XEnv env;
    env.add("a", up.port());
    env.add("b", good.port());
    env.start();
    ASSERT_TRUE(env.run().has_value());  // 建立一条连接，留在池中
    auto out = env.run(big);
    ASSERT_TRUE(out.has_value()) << out.error().message;
    EXPECT_EQ(out->header("X-LLMGW-Provider"), "a");
    EXPECT_EQ(env.results(), (std::vector{AttemptResult::kOk}));
    EXPECT_EQ(good.requests(), 0);
  }
  {
    TestUpstream up(UpstreamOptions{.rst_before_body = [](const Received&) { return true; }},
                    [](const Received&) { return ok("{}"); });
    TestUpstream good = ok_upstream();
    XEnv env;
    env.add("a", up.port());
    env.add("b", good.port());
    env.start();
    auto out = env.run(big);
    ASSERT_TRUE(out.has_value()) << out.error().message;
    EXPECT_EQ(env.results(), (std::vector{AttemptResult::kConnectError, AttemptResult::kOk}));
    EXPECT_EQ(good.requests(), 1);
  }
}

// ---------- X11：取消 ----------

TEST(Executor, X11_cancellation_stops_without_trying_the_next_target) {
  TestUpstream hang([](const Received&) {
    return Reply{.parts = {}, .close_after = false, .rst = false, .hang = true};
  });
  TestUpstream good = ok_upstream();
  XEnv env;
  env.add("a", hang.port());
  env.add("b", good.port());
  env.start();
  env.make_ctx(R"({"model":"smart"})", false, std::chrono::seconds(30));
  bool executor_won = true;
  const auto t0 = SteadyClock::now();
  executor_won = test::run(env.ioc, [&]() -> Task<bool> {
    asio::steady_timer t(env.ioc);
    t.expires_after(std::chrono::milliseconds(200));
    auto r =
        co_await (env.execute_and_drain() || t.async_wait(asio::as_tuple(asio::use_awaitable)));
    co_return r.index() == 0;
  }());
  const auto took = SteadyClock::now() - t0;
  EXPECT_FALSE(executor_won);
  EXPECT_LT(took, std::chrono::milliseconds(1000))
      << "取消没有立即生效（定时器 200 ms；留出 sanitizer 的余量）";
  EXPECT_EQ(good.requests(), 0) << "被取消：不再尝试下一个目标";
  EXPECT_EQ(env.results(), (std::vector{AttemptResult::kCancelled}));
  EXPECT_EQ(env.metrics.counter(Counter::kUpstreamAttempts, {"a", "cancelled"}), 1U);
  EXPECT_EQ(env.metrics.counter(Counter::kUpstreamAttempts, {"a", "timeout"}), 0U)
      << "不计为上游的失败";
  EXPECT_EQ(env.metrics.observed, 0U) << "被取消的尝试不记录耗时";
}

// ---------- X12：请求体不合法 ----------

TEST(Executor, X12_invalid_body_is_400_without_any_attempt) {
  TestUpstream good = ok_upstream();
  XEnv env;
  env.add("a", good.port());
  env.add("b", good.port());
  env.start();
  auto out = env.run("[1,2]");
  ASSERT_FALSE(out.has_value());
  EXPECT_EQ(out.error().code, ErrorCode::kInvalidRequest);
  EXPECT_EQ(good.requests(), 0);
  EXPECT_TRUE(env.ctx->attempts.empty());
}

// ---------- X13：每个目标按自己的规则改写 ----------

TEST(Executor, X13_each_target_rewrites_from_the_original_body) {
  TestUpstream first([](const Received&) { return status(503); });
  TestUpstream good = ok_upstream();
  XEnv env;
  env.add("a", first.port()).set_fields = {
      {.path = *config::parse_field_path("x"), .json_value = "1"}};
  env.add("b", good.port()).set_fields = {
      {.path = *config::parse_field_path("y"), .json_value = "2"}};
  env.start();
  auto out = env.run(R"({"model":"smart","keep":true})");
  ASSERT_TRUE(out.has_value());
  const Json sent_a = Json::parse(first.received().at(0).body);
  const Json sent_b = Json::parse(good.received().at(0).body);
  EXPECT_EQ(sent_a, (Json{{"model", "a-model"}, {"keep", true}, {"x", 1}}));
  EXPECT_EQ(sent_b, (Json{{"model", "b-model"}, {"keep", true}, {"y", 2}})) << "不能带着 a 的改写";
  EXPECT_EQ(first.received().at(0).header("Authorization"), "Bearer sk-SECRET-a");
  EXPECT_EQ(good.received().at(0).header("Authorization"), "Bearer sk-SECRET-b");
}

// ---------- X14：每次尝试之前重置 usage、ttft ----------

TEST(Executor, X14_usage_and_ttft_are_reset_per_attempt) {
  // a：200，响应体同时带业务错误码和 usage → 映射为 503（换下一个）。预读整个响应体时可能已经
  // 写入了 usage；另外在执行之前就往 ctx 里写入垃圾值（来自“之前的尝试”），两种来源都必须被清掉
  TestUpstream first([](const Received&) {
    return ok(
        R"({"base_resp":{"status_code":1002},"usage":{"prompt_tokens":999,"completion_tokens":999}})",
        "Content-Type: application/json\r\n");
  });
  TestUpstream good([](const Received&) {
    return chunked(
        {"data: {\"choices\":[{\"delta\":{\"content\":\"x\"}}]}\n\n", "data: [DONE]\n\n"});
  });
  XEnv env;
  env.add("a", first.port());
  env.config->providers[0].error_mapping = config::ErrorMapping{
      .field = *config::parse_field_path("base_resp.status_code"), .codes = {{"1002", 503}}};
  env.add("b", good.port());
  env.start();
  env.make_ctx(R"({"model":"smart","stream":true})", true, std::chrono::seconds(30));
  env.ctx->usage = Usage{
      .prompt_tokens = 777, .completion_tokens = 777, .reasoning_tokens = 0, .reported = true};
  env.ctx->ttft = std::chrono::hours(1);
  auto out = run(env.ioc, env.execute_and_drain(), std::chrono::seconds(20));
  ASSERT_TRUE(out.has_value()) << out.error().message;
  ASSERT_EQ(env.results(), (std::vector{AttemptResult::kStatus5xx, AttemptResult::kOk}))
      << "前提：第一个上游的业务错误码被映射为 503";
  EXPECT_EQ(env.ctx->usage.prompt_tokens, 0) << "不能残留之前的用量";
  EXPECT_FALSE(env.ctx->usage.reported);
  ASSERT_TRUE(env.ctx->ttft.has_value());
  EXPECT_LT(*env.ctx->ttft, std::chrono::seconds(10)) << "首字延迟来自 b，而不是残留的值";
}

// ---------- X15：每个目标的超时 ----------

TEST(Executor, X15_per_target_timeouts) {
  TestUpstream hang([](const Received&) {
    return Reply{.parts = {}, .close_after = false, .rst = false, .hang = true};
  });
  TestUpstream good = ok_upstream();
  XEnv env;
  env.config->retry.retry_on_ambiguous = true;
  env.add("a", hang.port()).timeouts.first_byte_non_stream = std::chrono::seconds(1);
  env.add("b", good.port()).timeouts.idle = std::chrono::seconds(7);
  env.start();
  const auto t0 = SteadyClock::now();
  auto out = env.run();
  const auto took = SteadyClock::now() - t0;
  ASSERT_TRUE(out.has_value());
  EXPECT_GE(took, std::chrono::milliseconds(900));
  EXPECT_LT(took, std::chrono::milliseconds(2500))
      << "用目标 a 自己的首字节超时，而不是默认的 300 秒";
  ASSERT_TRUE(env.ctx->upstream_idle.has_value());
  EXPECT_EQ(*env.ctx->upstream_idle, std::chrono::seconds(7)) << "选中的目标 b 的空闲超时";
}

// 独立审查 1.7 #2：配置了 error_mapping 时，响应头之后的预读也要受目标的空闲超时限制。
// 原来 send() 返回之后首字节时限不再生效、Session 的看门狗还没有启动，只能等总超时（600 秒）
TEST(Executor, X15_prefetch_for_error_mapping_is_bounded_by_the_idle_timeout) {
  for (const bool stream : {false, true}) {
    SCOPED_TRACE(stream ? "流式" : "非流式");
    TestUpstream stuck([stream](const Received&) {
      Reply r;
      r.parts = {Part{{},
                      stream ? "HTTP/1.1 200 OK\r\nContent-Type: "
                               "text/event-stream\r\nTransfer-Encoding: chunked\r\n\r\n"
                               "5\r\ndata:\r\n"
                             : "HTTP/1.1 200 OK\r\nContent-Type: "
                               "application/json\r\nContent-Length: 100\r\n\r\n{\"a\":"},
                 Part{std::chrono::hours(1), ""}};
      return r;
    });
    TestUpstream good = ok_upstream();
    XEnv env;
    auto& a = env.add("a", stuck.port());
    a.timeouts.idle = std::chrono::seconds(1);
    env.config->providers[0].error_mapping = config::ErrorMapping{
        .field = *config::parse_field_path("base_resp.status_code"), .codes = {}};
    env.add("b", good.port());
    env.start();
    const auto t0 = SteadyClock::now();
    auto out =
        env.run(stream ? R"({"model":"smart","stream":true})" : R"({"model":"smart"})", stream);
    const auto took = SteadyClock::now() - t0;
    EXPECT_LT(took, std::chrono::milliseconds(2500)) << "预读没有受空闲超时限制";
    ASSERT_FALSE(out.has_value());
    EXPECT_EQ(out.error().code, ErrorCode::kUpstreamTimeout)
        << "已经收到响应头：可能已经计费，不能重试";
    EXPECT_EQ(good.requests(), 0);
    EXPECT_EQ(env.results(), (std::vector{AttemptResult::kTimeout}));
  }
}

// ---------- X16：attempts 与指标 ----------

TEST(Executor, X16_attempts_and_metrics) {
  TestUpstream first([](const Received&) { return status(503); });
  TestUpstream good = ok_upstream();
  XEnv env;
  env.add("a", first.port());
  env.add("b", good.port());
  env.start();
  auto out = env.run();
  ASSERT_TRUE(out.has_value());
  ASSERT_EQ(env.ctx->attempts.size(), 2U);
  EXPECT_EQ(env.ctx->attempts[0].target, (Target{.provider = "a", .model = "a-model"}));
  EXPECT_EQ(env.ctx->attempts[1].target, (Target{.provider = "b", .model = "b-model"}));
  EXPECT_EQ(env.ctx->attempts[1].upstream_status, 200);
  EXPECT_GT(env.ctx->attempts[1].elapsed, SteadyClock::duration::zero());
  EXPECT_EQ(env.metrics.counter(Counter::kUpstreamAttempts, {"a", "5xx"}), 1U);
  EXPECT_EQ(env.metrics.counter(Counter::kUpstreamAttempts, {"b", "ok"}), 1U);
  EXPECT_EQ(env.metrics.dropped(), 0U);
  EXPECT_EQ(env.metrics.observed, 2U) << "每次尝试一个耗时";
  EXPECT_EQ(out->header("X-LLMGW-Upstream-Model"), "b-model");
  EXPECT_EQ(env.ctx->usage.prompt_tokens, 5);
}

}  // namespace
}  // namespace llmgw::test
