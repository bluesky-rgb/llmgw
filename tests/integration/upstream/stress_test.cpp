// 连接池的不变式：随机的上游行为 + 随机的取消，结束时名额全部归还、没有排队者、空闲连接不超过上限。
#include <boost/asio/as_tuple.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/experimental/awaitable_operators.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <random>

#include "harness.h"

namespace llmgw::test {
namespace {

using namespace asio::experimental::awaitable_operators;  // NOLINT(google-build-using-namespace)

Reply random_reply(const Received& r) {
  // 用请求体里的数字决定行为，保证同一个种子可以复现
  const int kind = std::stoi(r.body) % 7;
  switch (kind) {
    case 0:
      return ok("{}", "Connection: close\r\n");
    case 1:
      return Reply{.rst = true};
    case 2:
      return chunked({"a", "b", "c"}, 3ms);
    case 3:
      return Reply{.parts = {Part{{}, "garbage\r\n\r\n"}}, .close_after = true};
    case 4:
      return Reply{.parts = {Part{{}, "HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\nabc"}},
                   .close_after = true};
    default:
      return ok("{}");
  }
}

TEST(UpstreamStress, random_failures_and_cancellations_keep_the_pool_consistent) {
  TestUpstream up(UpstreamOptions{.idle_close = 30ms}, random_reply);
  Env env;
  constexpr std::uint32_t kLimit = 5;
  const auto* p = env.start(provider("http", "127.0.0.1", up.port(), kLimit));
  std::mt19937 rng(20261121);
  constexpr int kRequests = 300;
  int finished = 0;
  int cancelled = 0;
  run(
      env.ioc,
      [&]() -> Task<bool> {
        const auto ex = co_await asio::this_coro::executor;
        int remaining = kRequests;
        asio::steady_timer all_done(ex, SteadyClock::time_point::max());
        for (int i = 0; i < kRequests; ++i) {
          const int cancel_after_ms = static_cast<int>(rng() % 3 == 0 ? rng() % 30 : 10000);
          asio::co_spawn(
              ex,
              [&, i, cancel_after_ms]() -> Task<> {
                asio::steady_timer t(co_await asio::this_coro::executor,
                                     std::chrono::milliseconds(cancel_after_ms));
                auto r = co_await (
                    fetch(env.client.get(), p, post(std::to_string(i)), deadlines(2s, 2s)) ||
                    t.async_wait(asio::as_tuple(asio::use_awaitable)));
                (r.index() == 0 ? finished : cancelled) += 1;
                if (--remaining == 0) all_done.cancel();
              },
              asio::detached);
        }
        co_await all_done.async_wait(asio::as_tuple(asio::use_awaitable));
        co_return true;
      }(),
      60s);
  EXPECT_EQ(finished + cancelled, kRequests);
  const auto s = env.client->stats("up");
  EXPECT_EQ(s.busy, 0U) << "名额泄漏";
  EXPECT_EQ(s.waiting, 0U);
  EXPECT_LE(s.idle, kLimit);
  std::printf("  [完成 %d，取消 %d，上游连接 %d，上游完整处理 %d 个请求]\n", finished, cancelled,
              up.connections(), up.requests());
  // 之后的请求仍然正常
  EXPECT_TRUE(env.get(p, post("6")).ok());
}

}  // namespace
}  // namespace llmgw::test
