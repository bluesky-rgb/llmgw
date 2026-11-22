// U15：连接池省下了多少。本机 HTTPS 上游，复用连接 vs 每个请求新建连接（TCP + TLS 握手）。
// 本机回环几乎没有网络延迟，所以这里测到的只是 CPU 开销；真实网络上每次新建还要多 2–3 个 RTT。
#include <boost/asio/as_tuple.hpp>
#include <boost/asio/experimental/awaitable_operators.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>

#include "harness.h"

namespace llmgw::test {
namespace {

#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__) || !defined(NDEBUG)
constexpr bool kMeasurable = false;
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
constexpr bool kMeasurable = false;
#else
constexpr bool kMeasurable = true;
#endif
#else
constexpr bool kMeasurable = true;
#endif

double per_request_us(bool reuse, int n) {
  TestUpstream up(UpstreamOptions{.tls = TlsMode::kGood}, [reuse](const Received&) {
    return ok("{}", reuse ? "" : "Connection: close\r\n");
  });
  Env env;
  auto pr = provider("https", "localhost", up.port());
  pr.ca_file = test_pki().ca_file;
  const auto* p = env.start(pr);
  EXPECT_TRUE(env.get(p).ok());  // 预热（DNS、第一条连接）
  const auto t0 = std::chrono::steady_clock::now();
  for (int i = 0; i < n; ++i) EXPECT_TRUE(env.get(p).ok());
  const auto us =
      std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
  EXPECT_EQ(up.connections(), reuse ? 1 : n + 1);
  return us / n;
}

TEST(UpstreamPerf, connection_reuse_saves_the_handshake) {
  const double reused = per_request_us(true, 300);
  const double fresh = per_request_us(false, 300);
  std::printf("  [HTTPS 每个请求：复用连接 %.0f µs，新建连接 %.0f µs，%.1f 倍]\n", reused, fresh,
              fresh / reused);
  if (kMeasurable) {
    EXPECT_GT(fresh, reused * 2) << "复用连接应该明显更快";
  }
}

}  // namespace
}  // namespace llmgw::test

namespace llmgw::test {
namespace {

using namespace asio::experimental::awaitable_operators;  // NOLINT(google-build-using-namespace)

// 任务 1.9 压测发现：Body::read 每次先把调用者的缓冲区 resize 出 16 KB（resize
// 会清零），读完再缩回。 一个 SSE 事件只有一百多字节，每转发一块数据要清零约 40 KB：callgrind 中
// 58% 的指令是 memset。 修复后读进 Body 自己的、不初始化的缓冲区，只把读到的字节追加给调用者
TEST(UpstreamPerf, small_reads_do_not_grow_the_callers_buffer) {
  TestUpstream up([](const Received&) {
    return chunked(
        {"data: {\"choices\":[{\"delta\":{\"content\":\"x\"}}]}\n\n", "data: [DONE]\n\n"});
  });
  Env env;
  const auto* p = env.start(provider("http", "127.0.0.1", up.port()));
  const auto [capacity, body] = run(env.ioc, [&]() -> Task<std::pair<std::size_t, std::string>> {
    auto sent = co_await env.client->send(p, post(), deadlines());
    if (!sent) co_return std::pair<std::size_t, std::string>{0, "send 失败"};
    std::string out;
    std::size_t max_capacity = 0;
    for (;;) {
      auto more = co_await sent->body->read(&out);
      max_capacity = std::max(max_capacity, out.capacity());
      if (!more || !*more) break;
    }
    co_return std::pair{max_capacity, out};
  }());
  EXPECT_NE(body.find("[DONE]"), std::string::npos) << body;
  EXPECT_LT(capacity, 4096U) << "读一百多字节，调用者的缓冲区却被扩到了 16 KB（每次都要清零）";
}

// 1.4 独立审查
// #13：读取在等待时被取消，调用者的缓冲区保持原样。这里走的是“以错误码结束”的取消路径，
// 修复前后都通过：它防止改写读取逻辑时引入回归，不能单独证明这次的修复（审查 1.9 #8）
TEST(UpstreamPerf, cancelled_read_leaves_the_callers_buffer_untouched) {
  TestUpstream up([](const Received&) {
    Reply r;
    r.parts = {Part{{},
                    "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nTransfer-Encoding: "
                    "chunked\r\n\r\n"},
               Part{std::chrono::hours(1), ""}};  // 响应头之后不再发送
    return r;
  });
  Env env;
  const auto* p = env.start(provider("http", "127.0.0.1", up.port()));
  const std::string out = run(env.ioc, [&]() -> Task<std::string> {
    auto sent = co_await env.client->send(p, post(), deadlines());
    std::string buf = "before";
    if (!sent) co_return std::string("send 失败");
    asio::steady_timer t(env.ioc, std::chrono::milliseconds(100));
    (void)co_await (sent->body->read(&buf) || t.async_wait(asio::as_tuple(asio::use_awaitable)));
    co_return buf;
  }());
  EXPECT_EQ(out, "before");
}

}  // namespace
}  // namespace llmgw::test
