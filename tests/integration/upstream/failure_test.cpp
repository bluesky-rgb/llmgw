// 任务 1.4 验证项 U6–U8、U10–U12：连接失败、首字节超时、上游的坏响应、取消、代理、TLS 校验。
#include <sys/socket.h>

#include <boost/asio/as_tuple.hpp>
#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/experimental/awaitable_operators.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>

#include "harness.h"

namespace llmgw::test {
namespace {

using namespace asio::experimental::awaitable_operators;  // NOLINT(google-build-using-namespace)
using tcp = asio::ip::tcp;
using Clock = std::chrono::steady_clock;

// 一个没有监听的端口
std::uint16_t closed_port() {
  asio::io_context ioc;
  tcp::acceptor a(ioc, tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
  return a.local_endpoint().port();
}

// ---------- U6：连接失败 ----------

TEST(UpstreamConnect, refused_connection_is_not_sent) {
  Env env;
  const auto* p = env.start(provider("http", "127.0.0.1", closed_port()));
  const Fetched f = env.get(p);
  ASSERT_FALSE(f.ok());
  EXPECT_EQ(f.error->stage, Stage::kConnect);
  EXPECT_FALSE(f.error->maybe_sent);
  EXPECT_FALSE(f.error->timed_out);
  EXPECT_EQ(f.error->to_error().code, ErrorCode::kUpstreamUnavailable);
  EXPECT_EQ(env.client->stats("up").busy, 0U);
}

TEST(UpstreamConnect, stuck_dns_is_cut_off_by_the_connect_deadline) {
  // Asio 的 async_resolve 不能按操作取消；不做特殊处理的话，这里会一直等到解析返回（任务设计
  // 实现记录）
  upstream::ClientOptions opt;
  // 模拟真实的 getaddrinfo：3 秒后才返回，并且不响应取消（绑定一个空的取消槽）。
  // 反向验证时发现：最初用的是一个普通定时器，它本身可以被取消，测不出这个问题
  opt.resolver = [](std::string, std::uint16_t) -> Task<Result<std::vector<tcp::endpoint>>> {
    asio::steady_timer slow(co_await asio::this_coro::executor, 3s);
    co_await slow.async_wait(asio::bind_cancellation_slot(asio::cancellation_slot(),
                                                          asio::as_tuple(asio::use_awaitable)));
    co_return fail(ErrorCode::kUpstreamUnavailable, "解析超时");
  };
  Env env;
  const auto* p = env.start(provider("http", "stuck.test", 80), opt);
  const auto t0 = Clock::now();
  const Fetched f = env.get(p, post(), deadlines(200ms, 2s));
  const auto elapsed = Clock::now() - t0;
  ASSERT_FALSE(f.ok());
  EXPECT_EQ(f.error->stage, Stage::kResolve);
  EXPECT_TRUE(f.error->timed_out);
  EXPECT_LT(elapsed, 1s) << "连接时限没有生效";
  EXPECT_EQ(env.client->stats("up").busy, 0U);
}

TEST(UpstreamConnect, unanswered_syn_is_cut_off_by_the_connect_deadline) {
  // 一个 backlog 已满、从不 accept 的监听套接字：新的 SYN 被丢弃，connect 一直等待
  asio::io_context side;
  tcp::acceptor full(side);
  full.open(tcp::v4());
  full.bind(tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
  ::listen(full.native_handle(), 0);
  const auto port = full.local_endpoint().port();
  std::vector<std::unique_ptr<tcp::socket>> fillers;
  for (int i = 0; i < 4; ++i) {  // 填满 backlog
    auto s = std::make_unique<tcp::socket>(side);
    s->open(tcp::v4());
    s->non_blocking(true);
    // 直接调用系统的非阻塞 connect：Asio 的同步 connect 在 EINPROGRESS 之后仍会 poll 等待，backlog
    // 满时会卡住
    const tcp::endpoint ep(asio::ip::make_address("127.0.0.1"), port);
    (void)::connect(s->native_handle(), ep.data(), static_cast<socklen_t>(ep.size()));
    fillers.push_back(std::move(s));
  }
  Env env;
  const auto* p = env.start(provider("http", "127.0.0.1", port));
  const auto t0 = Clock::now();
  const Fetched f = env.get(p, post(), deadlines(300ms, 2s));
  const auto elapsed = Clock::now() - t0;
  ASSERT_FALSE(f.ok());
  if (f.error->stage == Stage::kConnect && !f.error->timed_out) {
    GTEST_SKIP() << "这个环境里 backlog 满时直接拒绝了连接，无法模拟无应答：" << f.error->detail;
  }
  EXPECT_EQ(f.error->stage, Stage::kConnect);
  EXPECT_TRUE(f.error->timed_out);
  EXPECT_FALSE(f.error->maybe_sent);
  EXPECT_LT(elapsed, 1s);
}

// ---------- U7：首字节超时 ----------

TEST(UpstreamTimeout, no_response_after_sending_is_maybe_sent) {
  TestUpstream up([](const Received&) { return Reply{.hang = true}; });
  Env env;
  const auto* p = env.start(provider("http", "127.0.0.1", up.port()));
  const auto t0 = Clock::now();
  const Fetched f = env.get(p, post(), deadlines(2s, 300ms));
  const auto elapsed = Clock::now() - t0;
  ASSERT_FALSE(f.ok());
  EXPECT_EQ(f.error->stage, Stage::kResponse);
  EXPECT_TRUE(f.error->maybe_sent) << "请求已经完整发出：上游可能正在生成并计费";
  EXPECT_TRUE(f.error->timed_out);
  EXPECT_EQ(f.error->to_error().code, ErrorCode::kUpstreamTimeout);
  EXPECT_LT(elapsed, 1s);
  EXPECT_EQ(env.client->stats("up").busy, 0U);
  EXPECT_EQ(env.client->stats("up").idle, 0U) << "超时的连接不能还回池中";
}

// ---------- U8：上游的坏响应 ----------

TEST(UpstreamBadResponse, malformed_heads_and_bodies_fail_cleanly) {
  struct Case {
    const char* name;
    std::string raw;
    Stage stage;
  };
  const std::vector<Case> cases = {
      {"不是 HTTP", "garbage\r\n\r\n", Stage::kResponse},
      {"响应头超过 64 KB", "HTTP/1.1 200 OK\r\nX-Big: " + std::string(70000, 'a') + "\r\n\r\n",
       Stage::kResponse},
      {"响应头中的裸 CR（注入）",
       "HTTP/1.1 200 OK\r\nX-A: 1\rX-Evil: 1\r\nContent-Length: 0\r\n\r\n", Stage::kResponse},
      {"状态码 600", "HTTP/1.1 600 X\r\nContent-Length: 0\r\n\r\n", Stage::kResponse},
      {"101 切换协议", "HTTP/1.1 101 Switching Protocols\r\nUpgrade: x\r\n\r\n", Stage::kResponse},
      {"响应体比长度短", "HTTP/1.1 200 OK\r\nContent-Length: 100\r\n\r\nshort", Stage::kBody},
      {"分块编码错误", "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\nzz\r\nxx\r\n",
       Stage::kBody},
  };
  for (const auto& c : cases) {
    SCOPED_TRACE(c.name);
    TestUpstream up(
        [&](const Received&) { return Reply{.parts = {Part{{}, c.raw}}, .close_after = true}; });
    Env env;
    const auto* p = env.start(provider("http", "127.0.0.1", up.port()));
    const Fetched f = env.get(p);
    ASSERT_FALSE(f.ok()) << "状态码 " << f.status;
    EXPECT_EQ(f.error->stage, c.stage) << f.error->detail;
    EXPECT_TRUE(f.error->maybe_sent);
    EXPECT_EQ(env.client->stats("up").busy, 0U);
    EXPECT_EQ(env.client->stats("up").idle, 0U);
  }
}

TEST(UpstreamBadResponse, informational_responses_are_skipped) {
  TestUpstream up([](const Received&) {
    return Reply{.parts = {Part{{}, "HTTP/1.1 100 Continue\r\n\r\n"},
                           Part{{}, "HTTP/1.1 103 Early Hints\r\nLink: x\r\n\r\n"},
                           Part{{}, "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\n{}"}}};
  });
  Env env;
  const auto* p = env.start(provider("http", "127.0.0.1", up.port()));
  const Fetched f = env.get(p);
  ASSERT_TRUE(f.ok()) << f.error->detail;
  EXPECT_EQ(f.status, 200);
  EXPECT_EQ(f.body, "{}");
}

TEST(UpstreamBadResponse, upstream_closing_before_any_response_on_reused_connection_is_flagged) {
  // 上游在第二个请求时一个字节不回就关闭：这是“上游恰好关闭空闲连接”的竞态的表现
  TestUpstream up([](const Received& r) {
    if (r.index == 2) return Reply{.close_after = true};
    return ok("{}");
  });
  Env env;
  const auto* p = env.start(provider("http", "127.0.0.1", up.port()));
  ASSERT_TRUE(env.get(p).ok());
  const Fetched f = env.get(p);
  ASSERT_FALSE(f.ok());
  EXPECT_EQ(f.error->stage, Stage::kResponse);
  EXPECT_TRUE(f.error->maybe_sent) << "写已经成功：默认按可能已发送处理";
  EXPECT_TRUE(f.error->reused_without_response);
}

// ---------- U10：取消 ----------

// 在竞速中取消 fetch（模拟客户端断开）。返回 fetch 是否在取消之前就结束了
Task<bool> fetch_cancelled_after(Client* client, const config::Provider* p,
                                 std::chrono::milliseconds after) {
  asio::steady_timer t(co_await asio::this_coro::executor, after);
  auto r = co_await (fetch(client, p, post(), deadlines(5s, 5s)) ||
                     t.async_wait(asio::as_tuple(asio::use_awaitable)));
  co_return r.index() == 0;
}

TEST(UpstreamCancel, cancellation_at_every_stage_releases_everything) {
  TestUpstream hang([](const Received&) { return Reply{.hang = true}; });
  TestUpstream slow_body([](const Received&) {
    Reply r = chunked({"a"});
    r.parts.back().delay = std::chrono::hours(1);  // 第一块之后不再有数据
    return r;
  });
  struct Case {
    const char* name;
    std::uint16_t port;
    std::uint32_t limit;
    int occupy;  ///< 先发几个请求占住连接（用于测试排队时取消）
  };
  for (const Case c :
       {Case{"等响应头时", hang.port(), 10, 0}, Case{"读响应体时", slow_body.port(), 10, 0},
        Case{"排队时", hang.port(), 1, 1}}) {
    SCOPED_TRACE(c.name);
    Env env;
    const auto* p = env.start(provider("http", "127.0.0.1", c.port, c.limit));
    const auto t0 = Clock::now();
    const bool finished = run(env.ioc, [&]() -> Task<bool> {
      if (c.occupy == 0) co_return co_await fetch_cancelled_after(env.client.get(), p, 200ms);
      auto [a, b] = co_await (fetch_cancelled_after(env.client.get(), p, 400ms) &&
                              fetch_cancelled_after(env.client.get(), p, 200ms));
      co_return a || b;
    }());
    EXPECT_FALSE(finished);
    EXPECT_LT(Clock::now() - t0, 2s) << "取消没有及时生效";
    const auto s = env.client->stats("up");
    EXPECT_EQ(s.busy, 0U) << "名额泄漏";
    EXPECT_EQ(s.waiting, 0U) << "排队者没有离开队列";
    EXPECT_EQ(s.idle, 0U) << "被取消的连接被还回了池中";
  }
}

TEST(UpstreamCancel, body_outliving_the_client_is_safe) {
  TestUpstream up([](const Received&) { return chunked({"a", "b"}, 50ms); });
  Env env;
  const auto* p = env.start(provider("http", "127.0.0.1", up.port()));
  std::unique_ptr<upstream::Body> body =
      run(env.ioc, [&]() -> Task<std::unique_ptr<upstream::Body>> {
        auto r = co_await env.client->send(p, post(), deadlines());
        co_return std::move(r->body);
      }());
  env.client.reset();  // Worker 退出时 Client 先销毁
  body.reset();        // 不能访问已经不存在的连接池（ASan 检查）
  SUCCEED();
}

// ---------- U11：代理 ----------

TEST(UpstreamProxy, https_through_connect_tunnel_with_auth_and_reuse) {
  TestUpstream up(UpstreamOptions{.tls = TlsMode::kGood}, [](const Received&) { return ok("{}"); });
  TestProxy proxy(ProxyOptions{.user = "alice", .password = "p@ss:w"});
  Env env;
  config::ProxyUrl pu{
      .url = {"http", "127.0.0.1", proxy.port(), ""}, .username = "alice", .password = "p@ss:w"};
  env.config->proxy.https = pu;
  auto pr = provider("https", "localhost", up.port());
  pr.ca_file = test_pki().ca_file;
  const auto* p = env.start(pr);
  for (int i = 0; i < 3; ++i) {
    const Fetched f = env.get(p);
    ASSERT_TRUE(f.ok()) << f.error->detail;
  }
  EXPECT_EQ(proxy.tunnels(), 1) << "隧道之上的 TLS 连接应该被复用";
  EXPECT_EQ(proxy.last_target(), "localhost:" + std::to_string(up.port()));
  EXPECT_EQ(up.received()[0].header("Proxy-Authorization"), "") << "代理的认证信息不能泄露给上游";
}

TEST(UpstreamProxy, http_through_absolute_form) {
  TestUpstream up([](const Received&) { return ok("{}"); });
  TestProxy proxy(ProxyOptions{.user = "bob", .password = "pw"});
  Env env;
  env.config->proxy.https = config::ProxyUrl{
      .url = {"http", "127.0.0.1", proxy.port(), ""}, .username = "bob", .password = "pw"};
  const auto* p = env.start(provider("http", "127.0.0.1", up.port()));
  const Fetched f = env.get(p);
  ASSERT_TRUE(f.ok()) << f.error->detail;
  EXPECT_EQ(proxy.forwards(), 1);
  EXPECT_EQ(proxy.last_target(),
            "http://127.0.0.1:" + std::to_string(up.port()) + "/v1/chat/completions");
  EXPECT_EQ(up.received()[0].header("Proxy-Authorization"), "");
}

TEST(UpstreamProxy, wrong_credentials_are_a_proxy_failure_without_the_password) {
  TestUpstream tls_up(UpstreamOptions{.tls = TlsMode::kGood},
                      [](const Received&) { return ok("{}"); });
  TestUpstream plain_up([](const Received&) { return ok("{}"); });
  TestProxy proxy(ProxyOptions{.user = "alice", .password = "right"});
  for (const bool https : {true, false}) {
    SCOPED_TRACE(https ? "CONNECT" : "绝对路径形式");
    Env env;
    env.config->proxy.https = config::ProxyUrl{.url = {"http", "127.0.0.1", proxy.port(), ""},
                                               .username = "alice",
                                               .password = "WrongSecret9"};
    auto pr =
        provider(https ? "https" : "http", "127.0.0.1", https ? tls_up.port() : plain_up.port());
    pr.ca_file = test_pki().ca_file;
    const auto* p = env.start(pr);
    const Fetched f = env.get(p);
    ASSERT_FALSE(f.ok());
    EXPECT_EQ(f.error->stage, Stage::kProxy);
    EXPECT_FALSE(f.error->maybe_sent);
    EXPECT_NE(f.error->detail.find("407"), std::string::npos) << f.error->detail;
    EXPECT_EQ(f.error->detail.find("WrongSecret9"), std::string::npos);
  }
  EXPECT_EQ(plain_up.requests(), 0) << "认证失败的请求不应该到达上游";
}

TEST(UpstreamProxy, direct_and_no_proxy_bypass_the_proxy) {
  TestUpstream up([](const Received&) { return ok("{}"); });
  TestProxy proxy;
  for (const bool use_no_proxy : {false, true}) {
    SCOPED_TRACE(use_no_proxy ? "NO_PROXY" : "direct");
    Env env;
    env.config->proxy.https = config::ProxyUrl{.url = {"http", "127.0.0.1", proxy.port(), ""}};
    if (use_no_proxy) {
      env.config->proxy.no_proxy = {"127.0.0.1"};
    } else {
      env.config->proxy.per_provider["up"] = std::nullopt;
    }
    const auto* p = env.start(provider("http", "127.0.0.1", up.port()));
    ASSERT_TRUE(env.get(p).ok());
  }
  EXPECT_EQ(proxy.forwards() + proxy.tunnels(), 0);
}

// ---------- U12：TLS 校验 ----------

TEST(UpstreamTls, untrusted_certificates_are_rejected) {
  struct Case {
    const char* name;
    TlsMode mode;
    bool use_test_ca;
    const char* host;
  };
  for (const Case c : {Case{"主机名不匹配", TlsMode::kWrongHost, true, "localhost"},
                       Case{"证书过期", TlsMode::kExpired, true, "localhost"},
                       Case{"自签名", TlsMode::kSelfSigned, true, "localhost"},
                       Case{"系统证书库不信任测试 CA", TlsMode::kGood, false, "localhost"},
                       Case{"IP 不在证书中", TlsMode::kWrongHost, true, "127.0.0.1"},
                       Case{"服务器只支持 TLS 1.1", TlsMode::kTls11Only, true, "localhost"}}) {
    SCOPED_TRACE(c.name);
    TestUpstream up(UpstreamOptions{.tls = c.mode}, [](const Received&) { return ok("{}"); });
    Env env;
    auto pr = provider("https", c.host, up.port());
    if (c.use_test_ca) pr.ca_file = test_pki().ca_file;
    const auto* p = env.start(pr);
    const Fetched f = env.get(p);
    ASSERT_FALSE(f.ok());
    EXPECT_EQ(f.error->stage, Stage::kTls) << f.error->detail;
    EXPECT_FALSE(f.error->maybe_sent);
    EXPECT_EQ(up.requests(), 0) << "证书不可信时不能发出请求";
    std::printf("  [%s] %s\n", c.name, f.error->detail.c_str());
  }
}

TEST(UpstreamTls, unreadable_ca_file_fails_at_startup) {
  config::Snapshot s;
  auto pr = provider("https", "localhost", 443);
  pr.ca_file = "/nonexistent/ca.pem";
  s.providers.push_back(pr);
  auto r = upstream::make_tls_contexts(s);
  ASSERT_FALSE(r.has_value());
  EXPECT_NE(r.error().message.find("/nonexistent/ca.pem"), std::string::npos);
}

}  // namespace
}  // namespace llmgw::test
