// 任务 1.4 验证项 U1–U5、U9、U13、U14：基本请求、连接复用、不复用的情况、失效连接、上限与排队、DNS
// 缓存、指标。
#include <boost/asio/experimental/awaitable_operators.hpp>
#include <thread>

#include "harness.h"

namespace llmgw::test {
namespace {

using namespace asio::experimental::awaitable_operators;  // NOLINT(google-build-using-namespace)

// ---------- U1：基本请求 ----------

TEST(UpstreamBasic, http_content_length_and_request_shape) {
  TestUpstream up([](const Received&) { return ok(R"({"id":"x"})", "X-Up: 1\r\n"); });
  Env env;
  const auto* p = env.start(provider("http", "127.0.0.1", up.port()));
  auto req = post();
  req.headers.push_back({"X-Custom", "v"});
  req.headers.push_back({"Host", "evil.example"});   // Client 自己设置，调用者的被忽略
  req.headers.push_back({"Content-Length", "999"});  // 同上
  req.headers.push_back({"X-Inject", "a\r\nX-Evil: 1"});  // 请求头注入：丢弃
  const Fetched f = env.get(p, req);
  ASSERT_TRUE(f.ok()) << f.error->detail;
  EXPECT_EQ(f.status, 200);
  EXPECT_EQ(f.body, R"({"id":"x"})");
  const auto rec = up.received();
  ASSERT_EQ(rec.size(), 1U);
  EXPECT_EQ(rec[0].method, "POST");
  EXPECT_EQ(rec[0].target, "/v1/chat/completions");
  EXPECT_EQ(rec[0].body, R"({"model":"m"})");
  EXPECT_EQ(rec[0].header("Host"), "127.0.0.1:" + std::to_string(up.port()));
  EXPECT_EQ(rec[0].header("Content-Length"), "13");
  EXPECT_EQ(rec[0].header("Accept-Encoding"), "identity");
  EXPECT_EQ(rec[0].header("Authorization"), "Bearer sk-test");
  EXPECT_EQ(rec[0].header("X-Custom"), "v");
  EXPECT_EQ(rec[0].header("X-Inject"), "");
  EXPECT_EQ(rec[0].header("X-Evil"), "");
}

TEST(UpstreamBasic, chunked_body_arrives_decoded_in_order) {
  TestUpstream up([](const Received&) {
    return chunked({"data: 1\n\n", "data: 2\n\n", "data: [DONE]\n\n"}, 5ms);
  });
  Env env;
  const auto* p = env.start(provider("http", "127.0.0.1", up.port()));
  const Fetched f = env.get(p);
  ASSERT_TRUE(f.ok()) << f.error->detail;
  EXPECT_EQ(f.body, "data: 1\n\ndata: 2\n\ndata: [DONE]\n\n");
}

TEST(UpstreamBasic, https_with_hostname_and_with_ip_address) {
  TestUpstream up(UpstreamOptions{.tls = TlsMode::kGood},
                  [](const Received&) { return chunked({"a", "b"}); });
  for (const char* host : {"localhost", "127.0.0.1"}) {  // 主机名：SNI + 按名字校验；IP：按 IP 校验
    SCOPED_TRACE(host);
    Env env;
    auto pr = provider("https", host, up.port());
    pr.ca_file = test_pki().ca_file;
    const auto* p = env.start(pr);
    const Fetched f = env.get(p);
    ASSERT_TRUE(f.ok()) << f.error->detail;
    EXPECT_EQ(f.body, "ab");
  }
}

// ---------- U2：连接复用 ----------

TEST(UpstreamReuse, sequential_requests_share_one_connection) {
  for (const TlsMode mode :
       {TlsMode::kNone, TlsMode::kGood}) {  // TLS：会话票据不能让复用前检查误判
    SCOPED_TRACE(mode == TlsMode::kGood ? "https" : "http");
    TestUpstream up(UpstreamOptions{.tls = mode}, [](const Received&) { return ok("{}"); });
    Env env;
    auto pr = provider(mode == TlsMode::kGood ? "https" : "http", "127.0.0.1", up.port());
    pr.ca_file = test_pki().ca_file;
    const auto* p = env.start(pr);
    for (int i = 0; i < 100; ++i) ASSERT_TRUE(env.get(p).ok()) << i;
    EXPECT_EQ(up.connections(), 1);
    EXPECT_EQ(up.requests(), 100);
    EXPECT_EQ(env.metrics.counter(Counter::kUpstreamConnections, {"up", "new"}), 1U);
    EXPECT_EQ(env.metrics.counter(Counter::kUpstreamConnections, {"up", "reused"}), 99U);
    EXPECT_EQ(env.client->stats("up").idle, 1U);
    EXPECT_EQ(env.client->stats("up").busy, 0U);
    EXPECT_EQ(env.metrics.gauge(Gauge::kPoolConnections, {"up", "idle"}), 1);  // U13
    EXPECT_EQ(env.metrics.gauge(Gauge::kPoolConnections, {"up", "busy"}), 0);
  }
}

TEST(UpstreamReuse, body_read_to_length_without_seeing_the_end_is_still_returned) {
  // 非流式：调用者按长度读够就丢弃 Body，没有再调用一次 read() 拿到 false
  TestUpstream up([](const Received&) { return ok("0123456789"); });
  Env env;
  const auto* p = env.start(provider("http", "127.0.0.1", up.port()));
  for (int i = 0; i < 3; ++i) {
    run(env.ioc, [&]() -> Task<bool> {
      auto r = co_await env.client->send(p, post(), deadlines());
      std::string body;
      while (body.size() < 10) {
        auto more = co_await r->body->read(&body);
        if (!more || !*more) break;
      }
      co_return true;  // 这里析构 Body
    }());
  }
  EXPECT_EQ(up.connections(), 1);
}

// ---------- U3：不复用的情况 ----------

TEST(UpstreamReuse, connections_that_must_not_be_reused) {
  struct Case {
    const char* name;
    Reply reply;
  };
  const std::vector<Case> cases = {
      {"Connection: close", ok("{}", "Connection: close\r\n")},
      {"HTTP/1.0", Reply{.parts = {Part{{}, "HTTP/1.0 200 OK\r\nContent-Length: 2\r\n\r\n{}"}}}},
      {"没有长度，以关闭表示结束",
       Reply{.parts = {Part{{}, "HTTP/1.1 200 OK\r\n\r\n{}"}}, .close_after = true}},
  };
  for (const auto& c : cases) {
    SCOPED_TRACE(c.name);
    TestUpstream up([&](const Received&) { return c.reply; });
    Env env;
    const auto* p = env.start(provider("http", "127.0.0.1", up.port()));
    for (int i = 0; i < 3; ++i) {
      const Fetched f = env.get(p);
      ASSERT_TRUE(f.ok()) << f.error->detail;
      EXPECT_EQ(f.body, "{}");
    }
    EXPECT_EQ(up.connections(), 3);
    EXPECT_EQ(env.client->stats("up").idle, 0U);
    EXPECT_EQ(env.client->stats("up").busy, 0U);
  }
}

TEST(UpstreamReuse, body_abandoned_midway_closes_the_connection) {
  TestUpstream up([](const Received&) { return chunked({"a", "b", "c"}, 50ms); });
  Env env;
  const auto* p = env.start(provider("http", "127.0.0.1", up.port()));
  for (int i = 0; i < 2; ++i) {
    run(env.ioc, [&]() -> Task<bool> {
      auto r = co_await env.client->send(p, post(), deadlines());
      std::string body;
      (void)co_await r->body->read(&body);  // 只读一块就放弃（客户端断开）
      co_return true;
    }());
    EXPECT_EQ(env.client->stats("up").busy, 0U) << "名额没有归还";
    EXPECT_EQ(env.client->stats("up").idle, 0U) << "没读完的连接被还回了池中";
  }
  EXPECT_EQ(up.connections(), 2);
}

// ---------- U4：失效连接 ----------

TEST(UpstreamStale, connection_closed_by_upstream_while_idle_is_detected_before_reuse) {
  TestUpstream up(UpstreamOptions{.idle_close = 100ms}, [](const Received&) { return ok("{}"); });
  Env env;
  const auto* p = env.start(provider("http", "127.0.0.1", up.port()));
  ASSERT_TRUE(env.get(p).ok());
  std::this_thread::sleep_for(300ms);  // 上游在 100 ms 后关闭了空闲连接
  const Fetched f = env.get(p);
  ASSERT_TRUE(f.ok()) << "复用了已经被关闭的连接：" << f.error->detail;
  EXPECT_EQ(up.connections(), 2);
  EXPECT_EQ(env.metrics.counter(Counter::kUpstreamConnections, {"up", "new"}), 2U);
}

TEST(UpstreamStale, write_failure_on_reused_connection_is_retried_once_on_a_new_one) {
  // 第一条连接上的第二个请求：上游读完请求头就 RST，请求体（16 MB，远大于套接字缓冲区）写不完。
  // 上游没有收到完整的请求，所以换新连接重发是安全的
  TestUpstream up(
      UpstreamOptions{
          .rst_before_body = [](const Received& r) { return r.connection == 1 && r.index == 2; }},
      [](const Received&) { return ok("{}"); });
  Env env;
  const auto* p = env.start(provider("http", "127.0.0.1", up.port()));
  ASSERT_TRUE(env.get(p).ok());
  const Fetched f = env.get(p, post(std::string(16U << 20, 'x')));
  ASSERT_TRUE(f.ok()) << f.error->detail;
  EXPECT_EQ(up.connections(), 2);
  EXPECT_EQ(up.requests(), 2) << "上游应该只完整处理了两个请求";
}

TEST(UpstreamStale, write_failure_on_new_connection_is_reported_as_not_sent) {
  TestUpstream up(UpstreamOptions{.rst_before_body = [](const Received&) { return true; }},
                  [](const Received&) { return ok("{}"); });
  Env env;
  const auto* p = env.start(provider("http", "127.0.0.1", up.port()));
  const Fetched f = env.get(p, post(std::string(16U << 20, 'x')));
  ASSERT_FALSE(f.ok());
  EXPECT_EQ(f.error->stage, Stage::kSend);
  EXPECT_FALSE(f.error->maybe_sent) << "请求没有完整发出，上游不可能处理";
  EXPECT_EQ(f.error->to_error().code, ErrorCode::kUpstreamUnavailable);
  EXPECT_EQ(up.connections(), 1) << "新连接上写失败不自动重试";
}

// ---------- U5：上限与排队 ----------

TEST(UpstreamLimit, requests_beyond_the_limit_wait_for_a_free_connection) {
  TestUpstream up([](const Received&) {
    Reply r = ok("{}");
    r.parts[0].delay = 200ms;
    return r;
  });
  Env env;
  const auto* p = env.start(provider("http", "127.0.0.1", up.port(), 2));
  const auto t0 = std::chrono::steady_clock::now();
  const auto results = run(env.ioc, [&]() -> Task<std::array<Fetched, 3>> {
    auto [a, b, c] =
        co_await (fetch(env.client.get(), p, post()) && fetch(env.client.get(), p, post()) &&
                  fetch(env.client.get(), p, post()));
    co_return std::array<Fetched, 3>{a, b, c};
  }());
  const auto elapsed = std::chrono::steady_clock::now() - t0;
  for (const auto& f : results) EXPECT_TRUE(f.ok()) << f.error->detail;
  EXPECT_EQ(up.connections(), 2) << "超过上限新建了连接";
  EXPECT_GE(elapsed, 380ms) << "第 3 个请求应该等前一个结束";
  EXPECT_EQ(env.client->stats("up").waiting, 0U);
  EXPECT_EQ(env.client->stats("up").busy, 0U);
}

TEST(UpstreamLimit, queue_timeout_is_a_503_that_never_reached_the_upstream) {
  TestUpstream up([](const Received&) { return Reply{.hang = true}; });
  Env env;
  const auto* p = env.start(provider("http", "127.0.0.1", up.port(), 1));
  const auto results = run(env.ioc, [&]() -> Task<std::array<Fetched, 2>> {
    // 第一个请求占住唯一的连接（上游不回复，first_byte 1 秒）；第二个排队，connect 时限 200 ms
    auto [a, b] = co_await (fetch(env.client.get(), p, post(), deadlines(2s, 1s)) &&
                            fetch(env.client.get(), p, post(), deadlines(200ms, 2s)));
    co_return std::array<Fetched, 2>{a, b};
  }());
  ASSERT_FALSE(results[1].ok());
  const AttemptError& e = *results[1].error;
  EXPECT_EQ(e.stage, Stage::kQueue);
  EXPECT_TRUE(e.timed_out);
  EXPECT_FALSE(e.maybe_sent);
  EXPECT_EQ(e.to_error().code, ErrorCode::kOverloaded);
  EXPECT_EQ(env.metrics.counter(Counter::kUpstreamConnections, {"up", "queue_timeout"}), 1U);
  EXPECT_EQ(env.client->stats("up").waiting, 0U);
  EXPECT_EQ(env.client->stats("up").busy, 0U);
}

// ---------- U9：DNS 缓存 ----------

TEST(UpstreamDns, results_are_cached_and_stale_results_survive_failures) {
  TestUpstream up(
      [](const Received&) { return ok("{}", "Connection: close\r\n"); });  // 每个请求都新建连接
  int calls = 0;
  bool failing = false;
  upstream::ClientOptions opt;
  opt.dns_ttl = 300ms;
  opt.resolver = [&](std::string host,
                     std::uint16_t port) -> Task<Result<std::vector<asio::ip::tcp::endpoint>>> {
    ++calls;
    EXPECT_EQ(host, "api.fake.test");
    if (failing) co_return fail(ErrorCode::kUpstreamUnavailable, "模拟 DNS 故障");
    co_return std::vector<asio::ip::tcp::endpoint>{{asio::ip::make_address("127.0.0.1"), port}};
  };
  Env env;
  const auto* p = env.start(provider("http", "api.fake.test", up.port()), opt);
  for (int i = 0; i < 3; ++i) ASSERT_TRUE(env.get(p).ok());
  EXPECT_EQ(calls, 1) << "TTL 内应该只解析一次";
  std::this_thread::sleep_for(400ms);
  ASSERT_TRUE(env.get(p).ok());
  EXPECT_EQ(calls, 2) << "过期后应该重新解析";
  EXPECT_EQ(up.received().back().header("Host"), "api.fake.test:" + std::to_string(up.port()));
  failing = true;
  std::this_thread::sleep_for(400ms);
  const Fetched f = env.get(p);
  EXPECT_TRUE(f.ok()) << "重新解析失败时应该继续用旧结果：" << (f.ok() ? "" : f.error->detail);
  EXPECT_EQ(calls, 3);
}

// ---------- U14：每个 Worker 的上限 ----------

TEST(UpstreamLimit, per_worker_limit_divides_the_total) {
  EXPECT_EQ(upstream::per_worker_limit(256, 8), 32U);
  EXPECT_EQ(upstream::per_worker_limit(256, 3), 86U);
  EXPECT_EQ(upstream::per_worker_limit(256, 1000), 1U);
  EXPECT_EQ(upstream::per_worker_limit(1, 0), 1U);
}

TEST(UpstreamErrors, attempt_error_maps_to_client_error) {
  EXPECT_EQ((AttemptError{.stage = Stage::kQueue}).to_error().code, ErrorCode::kOverloaded);
  EXPECT_EQ((AttemptError{.stage = Stage::kConnect}).to_error().code,
            ErrorCode::kUpstreamUnavailable);
  EXPECT_EQ((AttemptError{.stage = Stage::kResponse, .maybe_sent = true}).to_error().code,
            ErrorCode::kUpstreamTimeout);
  EXPECT_FALSE(
      should_retry((AttemptError{.stage = Stage::kBody, .maybe_sent = true}).to_error().code))
      << "可能已经计费：不能建议客户端重试";
}

}  // namespace
}  // namespace llmgw::test
