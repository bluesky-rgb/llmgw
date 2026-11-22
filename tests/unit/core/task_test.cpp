// is_cancelled(co_await kCancellationState)：被取消之后检查返回 true，并且检查本身不抛异常。
// 同时记录为什么不能把检查写成一个协程（任务 1.7 发现的潜在问题）。
#include "llmgw/core/task.h"

#include <gtest/gtest.h>

#include <boost/asio/as_tuple.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/experimental/awaitable_operators.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <chrono>

namespace llmgw {
namespace {

namespace asio = boost::asio;
using namespace asio::experimental::awaitable_operators;  // NOLINT(google-build-using-namespace)

struct Seen {
  bool checked = false;    ///< 检查执行完了（没有抛异常）
  bool cancelled = false;  ///< 检查的结果
  bool threw = false;
};

Task<bool> as_coroutine() {  // 旧的写法：检查本身是一个协程
  co_return is_cancelled(co_await kCancellationState);
}

// 等一个长时间的定时器（会被取消），然后检查取消状态
Task<void> victim(Seen* seen, bool old_style) {
  asio::steady_timer t(co_await asio::this_coro::executor);
  t.expires_after(std::chrono::hours(1));
  co_await t.async_wait(asio::as_tuple(asio::use_awaitable));  // 被取消时以错误码结束，不抛异常
  try {
    seen->cancelled =
        old_style ? co_await as_coroutine() : is_cancelled(co_await kCancellationState);
    seen->checked = true;
  } catch (const std::exception&) {
    seen->threw = true;
  }
}

Seen run_race(bool old_style) {
  asio::io_context ioc;
  Seen seen;
  asio::co_spawn(
      ioc,
      [&]() -> Task<void> {
        asio::steady_timer t(ioc);
        t.expires_after(std::chrono::milliseconds(10));
        co_await (victim(&seen, old_style) || t.async_wait(asio::use_awaitable));
      },
      asio::detached);
  ioc.run();
  return seen;
}

TEST(Task, is_cancelled_after_cancellation_returns_true_without_throwing) {
  const Seen s = run_race(false);
  EXPECT_TRUE(s.checked);
  EXPECT_TRUE(s.cancelled);
  EXPECT_FALSE(s.threw);
}

TEST(Task, checking_inside_a_nested_coroutine_throws_instead) {
  // 这就是 server 中原来的 is_cancelled() 的行为：检查永远返回不了 true
  const Seen s = run_race(true);
  EXPECT_FALSE(s.checked);
  EXPECT_TRUE(s.threw);
}

TEST(Task, not_cancelled_is_false) {
  asio::io_context ioc;
  bool cancelled = true;
  asio::co_spawn(
      ioc, [&]() -> Task<void> { cancelled = is_cancelled(co_await kCancellationState); },
      asio::detached);
  ioc.run();
  EXPECT_FALSE(cancelled);
}

}  // namespace
}  // namespace llmgw
