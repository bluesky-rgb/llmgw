// U15：连接池省下了多少。本机 HTTPS 上游，复用连接 vs 每个请求新建连接（TCP + TLS 握手）。
// 本机回环几乎没有网络延迟，所以这里测到的只是 CPU 开销；真实网络上每次新建还要多 2–3 个 RTT。
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
