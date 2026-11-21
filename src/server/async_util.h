// 内部：Session 和控制面共用的异步辅助函数与别名。
#pragma once

#include <sys/socket.h>

#include <boost/asio.hpp>
#include <boost/asio/experimental/awaitable_operators.hpp>
#include <boost/beast.hpp>
#include <chrono>

#include "llmgw/core/request_context.h"
#include "llmgw/core/task.h"

namespace llmgw::server::detail {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
using tcp = asio::ip::tcp;
using namespace asio::experimental::awaitable_operators;  // NOLINT(google-build-using-namespace)
                                                          // 提供 a || b

/// 所有异步操作都用这个完成令牌：出错时返回错误码，而不是抛异常。
inline constexpr auto kTuple = asio::as_tuple(asio::use_awaitable);

/// 等待定时器到期。被取消时（竞速中别的分支先结束）也直接返回。
inline Task<> timer_fired(asio::steady_timer* t) { co_await t->async_wait(kTuple); }

/// 一直等待，直到被取消。
inline Task<> forever() {
  asio::steady_timer t(co_await asio::this_coro::executor, asio::steady_timer::time_point::max());
  co_await t.async_wait(kTuple);
}

/// 等待客户端关闭连接。
///
/// 在连接上等待“可读”。可读时用 MSG_PEEK 看一个字节：
/// - 0 或出错：对方关闭了连接（或者连接被重置）→ 返回
/// - 有数据：客户端提前发来了下一个请求（HTTP 管线化），不算断开。停止检测、一直等待，
///   数据留在内核缓冲区里给下一次读取（0.6 demo 把这种情况误判为断开）
///
/// 已知限制：只关闭写方向（半关闭）的客户端会被当作断开。HTTP 客户端几乎不这样做。
inline Task<> client_closed(tcp::socket* socket) {
  auto [ec] = co_await socket->async_wait(tcp::socket::wait_read, kTuple);
  if (ec) co_return;
  char byte = 0;
  const auto n = ::recv(socket->native_handle(), &byte, 1, MSG_PEEK | MSG_DONTWAIT);
  if (n <= 0) co_return;
  co_await forever();
}

/// 惰性空闲定时器（ADR-006 结论 2）：有进展时只更新 *last_activity，不重设定时器；
/// 定时器到期时如果期间有过进展，就顺延到 last_activity + idle。连续 idle 没有进展时返回。
inline Task<> idle_watch(asio::steady_timer* t, const SteadyClock::time_point* last_activity,
                         SteadyClock::duration idle) {
  for (;;) {
    t->expires_at(*last_activity + idle);
    auto [ec] = co_await t->async_wait(kTuple);
    if (ec) co_return;  // 被取消：别的分支已经先结束了
    if (SteadyClock::now() - *last_activity >= idle) co_return;
  }
}

}  // namespace llmgw::server::detail
