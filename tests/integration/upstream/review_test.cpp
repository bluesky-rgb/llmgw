// 任务 1.4 独立审查发现的问题（R1–R12）。每个用例先复现问题（修复前失败），再保护修复。
#include <arpa/inet.h>
#include <netinet/in.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <boost/asio/as_tuple.hpp>
#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/experimental/awaitable_operators.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <mutex>
#include <optional>
#include <sstream>
#include <thread>

#include "establish.h"
#include "harness.h"
#include "llmgw/config/loader.h"
#include "message.h"
#include "pool.h"

namespace llmgw::test {
namespace {

using namespace asio::experimental::awaitable_operators;  // NOLINT(google-build-using-namespace)
using tcp = asio::ip::tcp;
using Endpoints = std::vector<tcp::endpoint>;

int listen_local(std::uint16_t* port) {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  const int one = 1;
  ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  ::bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof a);
  ::listen(fd, 16);
  socklen_t len = sizeof a;
  ::getsockname(fd, reinterpret_cast<sockaddr*>(&a), &len);
  *port = ntohs(a.sin_port);
  return fd;
}

void load_cert(SSL_CTX* ctx, const CertFiles& f) {
  BIO* b = BIO_new_mem_buf(f.pem_cert.data(), static_cast<int>(f.pem_cert.size()));
  X509* x = PEM_read_bio_X509(b, nullptr, nullptr, nullptr);
  SSL_CTX_use_certificate(ctx, x);
  X509_free(x);
  BIO_free(b);
  b = BIO_new_mem_buf(f.pem_key.data(), static_cast<int>(f.pem_key.size()));
  EVP_PKEY* k = PEM_read_bio_PrivateKey(b, nullptr, nullptr, nullptr);
  SSL_CTX_use_PrivateKey(ctx, k);
  EVP_PKEY_free(k);
  BIO_free(b);
}

// 阻塞式 TLS 上游：第一条连接的第一个请求，把两个响应作为两个 TLS 记录、用一次 send() 写出
// （第二个是上游多发的）。其他请求正常回复 "fresh"
class TwoRecordTlsServer {
 public:
  TwoRecordTlsServer() : ctx_(SSL_CTX_new(TLS_server_method())) {
    load_cert(ctx_, test_pki().good);
    lfd_ = listen_local(&port_);
    accept_thread_ = std::thread([this] { accept_loop(); });
  }
  ~TwoRecordTlsServer() {
    ::shutdown(lfd_, SHUT_RDWR);
    ::close(lfd_);
    accept_thread_.join();
    for (auto& t : serving_) t.join();  // 客户端（Env）先销毁，连接已关闭，服务线程会退出
    SSL_CTX_free(ctx_);
  }
  TwoRecordTlsServer(const TwoRecordTlsServer&) = delete;
  TwoRecordTlsServer& operator=(const TwoRecordTlsServer&) = delete;
  TwoRecordTlsServer(TwoRecordTlsServer&&) = delete;
  TwoRecordTlsServer& operator=(TwoRecordTlsServer&&) = delete;
  [[nodiscard]] std::uint16_t port() const { return port_; }

 private:
  void accept_loop() {
    for (int id = 1;; ++id) {
      const int c = ::accept(lfd_, nullptr, nullptr);
      if (c < 0) return;
      const std::lock_guard lock(mu_);
      serving_.emplace_back([this, c, id] { serve(c, id); });
    }
  }
  void serve(int c, int conn_id) {
    SSL* s = SSL_new(ctx_);
    SSL_set_fd(s, c);
    if (SSL_accept(s) == 1) {
      std::string in;
      for (int idx = 1; read_request(s, &in); ++idx) respond(s, c, conn_id == 1 && idx == 1);
    }
    SSL_free(s);
    ::close(c);
  }
  static bool read_request(SSL* s, std::string* in) {
    for (;;) {
      const auto h = in->find("\r\n\r\n");
      if (h != std::string::npos) {
        std::size_t cl = 0;
        if (auto p = in->find("Content-Length: "); p != std::string::npos && p < h)
          cl = std::stoul(in->substr(p + 16));
        if (in->size() >= h + 4 + cl) {
          in->erase(0, h + 4 + cl);
          return true;
        }
      }
      char buf[4096];
      const int n = SSL_read(s, buf, sizeof buf);
      if (n <= 0) return false;
      in->append(buf, static_cast<std::size_t>(n));
    }
  }
  static void respond(SSL* s, int fd, bool two_records) {
    if (!two_records) {
      const std::string r = "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nfresh";
      SSL_write(s, r.data(), static_cast<int>(r.size()));
      return;
    }
    BIO* mem = BIO_new(BIO_s_mem());
    SSL_set0_wbio(s, mem);
    const std::string r1 = "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nfirst";
    const std::string r2 = "HTTP/1.1 200 OK\r\nContent-Length: 6\r\n\r\nSTOLEN";
    SSL_write(s, r1.data(), static_cast<int>(r1.size()));
    SSL_write(s, r2.data(), static_cast<int>(r2.size()));
    char* data = nullptr;
    const long len = BIO_get_mem_data(mem, &data);
    ::send(fd, data, static_cast<std::size_t>(len), 0);  // 一次 send，两个记录
    SSL_set0_wbio(s, BIO_new_socket(fd, BIO_NOCLOSE));
  }

  SSL_CTX* ctx_;
  int lfd_ = -1;
  std::uint16_t port_ = 0;
  std::thread accept_thread_;
  std::mutex mu_;
  std::vector<std::thread> serving_;
};

// 模拟真实的 getaddrinfo：慢，且不响应取消
Task<Result<Endpoints>> slow_uncancellable(std::chrono::milliseconds d, std::uint16_t port) {
  asio::steady_timer slow(co_await asio::this_coro::executor, d);
  co_await slow.async_wait(
      asio::bind_cancellation_slot(asio::cancellation_slot(), asio::as_tuple(asio::use_awaitable)));
  co_return Endpoints{tcp::endpoint(asio::ip::make_address("127.0.0.1"), port)};
}

// R1（严重）：TLS
// 层还有未读的记录（上游多发了一个响应）时，连接被复用，下一个请求收到了不属于它的响应
TEST(UpstreamReview, tls_connection_with_unread_records_is_not_reused) {
  for (int i = 0; i < 20; ++i) {
    TwoRecordTlsServer srv;
    Env env;
    auto pr = provider("https", "localhost", srv.port());
    pr.ca_file = test_pki().ca_file;
    const auto* p = env.start(pr);
    const Fetched a = env.get(p);
    ASSERT_TRUE(a.ok()) << a.error->detail;
    EXPECT_EQ(a.body, "first");
    const Fetched b = env.get(p);
    ASSERT_TRUE(b.ok()) << b.error->detail;
    ASSERT_EQ(b.body, "fresh") << "第 " << i << " 次：第二个请求收到了上游多发的响应";
  }
}

// R2（严重）：排队者已被交接名额、但协程帧在恢复之前就被销毁（Worker
// 退出）：重复出队（use-after-free）、名额泄漏
TEST(UpstreamReview, granted_waiter_destroyed_without_resuming_returns_the_slot) {
  auto pool = std::make_shared<upstream::detail::Pool>("p", 1, nullptr);
  {
    auto ioc = std::make_unique<asio::io_context>(1);
    ASSERT_TRUE(pool->try_take().has_value());
    auto lease = std::make_unique<upstream::detail::Lease>(pool, nullptr);
    asio::co_spawn(*ioc, upstream::detail::acquire(pool), asio::detached);
    ioc->run_for(20ms);
    ASSERT_EQ(pool->waiting_count(), 1U);
    lease.reset();  // 名额交给排队者，唤醒的回调已经投递但还没有执行
    EXPECT_EQ(pool->busy_count(), 1U);
    ioc.reset();  // Worker 退出：回调和协程帧被直接销毁
  }
  EXPECT_EQ(pool->busy_count(), 0U) << "交接给排队者的名额泄漏了";
  EXPECT_EQ(pool->waiting_count(), 0U);
}

// R4（主要）：方法和目标中的 CR/LF 可以注入请求行（请求走私）
TEST(UpstreamReview, invalid_method_or_target_is_rejected_before_sending) {
  config::Provider prov = provider("http", "example.com", 80);
  const upstream::detail::Route route{.provider = &prov, .proxy = nullptr, .tls = nullptr};
  for (const auto& [method, target] : std::vector<std::pair<std::string, std::string>>{
           {"GET / HTTP/1.1\r\nX-Smuggled: 1\r\n\r\nPOST", "/v1/x"},
           {"POST", "/v1/x HTTP/1.1\r\nX-Injected: yes\r\nY:"},
           {"POST", "/v1/a b"},
           {"POST", "v1/no-leading-slash"},
           {"POST", ""},
           {"", "/v1/x"}}) {
    SCOPED_TRACE(method + " " + target);
    upstream::Request req;
    req.method = method;
    req.target = target;
    auto msg = upstream::detail::build_message(route, std::move(req));
    ASSERT_FALSE(msg.has_value()) << "不合法的方法或目标被发出";
    EXPECT_EQ(msg.error().stage, Stage::kSend);
    EXPECT_FALSE(msg.error().maybe_sent);
  }
  upstream::Request good;
  good.method = "POST";
  good.target = "/v1/chat/completions?x=1&y=%20";
  EXPECT_TRUE(upstream::detail::build_message(route, std::move(good)).has_value());
}

// R5（主要，以后的调用者会遇到）：HEAD 的响应没有响应体，但按 Content-Length 一直等
TEST(UpstreamReview, head_response_has_no_body) {
  TestUpstream up([](const Received&) {
    return Reply{.parts = {Part{{}, "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\n"}}};
  });
  Env env;
  const auto* p = env.start(provider("http", "127.0.0.1", up.port()));
  upstream::Request r = post("");
  r.method = "HEAD";
  for (int i = 0; i < 2; ++i) {  // 第二次：连接被正确复用
    const Fetched f = run(env.ioc, fetch(env.client.get(), p, r), 2s);
    ASSERT_TRUE(f.ok()) << f.error->detail;
    EXPECT_EQ(f.body, "");
  }
  EXPECT_EQ(up.connections(), 1);
}

// R9（主要）：DNS 过期后，重新解析很慢时没有先用旧结果（design.md 5.5“失败时继续用旧结果”）
TEST(UpstreamReview, stale_dns_entry_is_used_while_refreshing) {
  TestUpstream up([](const Received&) { return ok("{}", "Connection: close\r\n"); });
  int calls = 0;
  upstream::ClientOptions opt;
  const std::uint16_t port = up.port();
  opt.dns_ttl = 50ms;
  opt.resolver = [&calls, port](std::string, std::uint16_t) -> Task<Result<Endpoints>> {
    return slow_uncancellable(++calls == 1 ? 0ms : 3000ms, port);
  };
  Env env;
  const auto* p = env.start(provider("http", "api.fake.test", port), opt);
  ASSERT_TRUE(env.get(p).ok());
  std::this_thread::sleep_for(100ms);  // 缓存过期
  for (int i = 0; i < 3; ++i) {
    const Fetched f = env.get(p, post(), deadlines(500ms, 1s));
    ASSERT_TRUE(f.ok()) << "有旧结果时不应该等待重新解析：" << f.error->detail;
  }
  EXPECT_EQ(calls, 2) << "后台只应该有一个刷新";
}

// R10（主要）：同一个主机的并发解析没有合并（DNS 故障时被放弃的解析会无限堆积）
TEST(UpstreamReview, concurrent_cold_resolves_are_shared) {
  TestUpstream up([](const Received&) { return ok("{}"); });
  int calls = 0;
  upstream::ClientOptions opt;
  const std::uint16_t port = up.port();
  opt.resolver = [&calls, port](std::string, std::uint16_t) -> Task<Result<Endpoints>> {
    ++calls;
    return slow_uncancellable(100ms, port);
  };
  Env env;
  const auto* p = env.start(provider("http", "api.fake.test", port), opt);
  const auto all = run(env.ioc, [&]() -> Task<std::array<Fetched, 4>> {
    auto [a, b, c, d] =
        co_await (fetch(env.client.get(), p, post()) && fetch(env.client.get(), p, post()) &&
                  fetch(env.client.get(), p, post()) && fetch(env.client.get(), p, post()));
    co_return std::array<Fetched, 4>{a, b, c, d};
  }());
  for (const auto& f : all) EXPECT_TRUE(f.ok());
  EXPECT_EQ(calls, 1);
}

TEST(UpstreamReview, failed_resolution_is_briefly_remembered) {
  int calls = 0;
  upstream::ClientOptions opt;
  opt.resolver = [&calls](std::string, std::uint16_t) -> Task<Result<Endpoints>> {
    ++calls;
    co_return fail(ErrorCode::kUpstreamUnavailable, "NXDOMAIN");
  };
  Env env;
  const auto* p = env.start(provider("http", "missing.test", 80), opt);
  for (int i = 0; i < 5; ++i) {
    const Fetched f = env.get(p);
    ASSERT_FALSE(f.ok());
    EXPECT_EQ(f.error->stage, Stage::kResolve);
  }
  EXPECT_EQ(calls, 1) << "短时间内的失败应该被记住，避免每个请求都去解析";
}

// R11（主要）：HTTP 上游经过代理时，上游自己返回的 407 被当作代理的拒绝（maybe_sent =
// false），可能重复扣费
TEST(UpstreamReview, upstream_407_through_proxy_is_a_normal_response) {
  TestUpstream up([](const Received&) {
    return Reply{
        .parts = {
            Part{{}, "HTTP/1.1 407 Proxy Authentication Required\r\nContent-Length: 0\r\n\r\n"}}};
  });
  TestProxy proxy;
  Env env;
  env.config->proxy.https = config::ProxyUrl{.url = {"http", "127.0.0.1", proxy.port(), ""}};
  const auto* p = env.start(provider("http", "127.0.0.1", up.port()));
  const Fetched f = env.get(p);
  ASSERT_TRUE(f.ok()) << "上游已经处理了请求，不能报告为代理拒绝：" << f.error->detail;
  EXPECT_EQ(f.status, 407);
  EXPECT_EQ(up.requests(), 1);
}

// R12（主要）：带 zone id 的 IPv6 地址（fe80::1%eth0）让证书身份校验被静默关闭
TEST(UpstreamReview, tls_identity_setup_fails_closed) {
  SSL_CTX* ctx = SSL_CTX_new(TLS_client_method());
  for (const char* host : {"fe80::1%eth0", "::1%lo"}) {
    SSL* s = SSL_new(ctx);
    EXPECT_FALSE(upstream::detail::set_tls_identity(s, host)) << host;
    SSL_free(s);
  }
  for (const char* host : {"localhost", "127.0.0.1", "::1", "api.openai.com"}) {
    SSL* s = SSL_new(ctx);
    EXPECT_TRUE(upstream::detail::set_tls_identity(s, host)) << host;
    SSL_free(s);
  }
  SSL_CTX_free(ctx);
  EXPECT_FALSE(config::parse_url("https://[fe80::1%25eth0]:443").has_value())
      << "配置中不允许 zone id";
}

}  // namespace
}  // namespace llmgw::test
