// Task<T>：所有异步函数的返回类型。
//
// 这是 core 中唯一引用 Boost.Asio 的地方（ADR-007）。其他头文件只使用 Task，
// 不直接出现 boost::asio 的其他类型；将来如果更换协程库，只需修改这一个别名。
#pragma once

#include <boost/asio/awaitable.hpp>
#include <boost/asio/cancellation_state.hpp>
#include <boost/asio/cancellation_type.hpp>
#include <boost/asio/this_coro.hpp>

// 所有使用 llmgw 头文件的编译单元都必须定义 BOOST_ASIO_NO_DEPRECATED（根 CMakeLists.txt
// 中统一定义）。 它会改变 Asio 成员函数的返回类型，部分编译单元定义、部分不定义会造成 ODR 违规：
// 链接器只保留一个版本，其他调用方按错误的调用约定调用（任务 1.4 中 UBSan 发现 this = 0x2）
#if !defined(BOOST_ASIO_NO_DEPRECATED)
#error "BOOST_ASIO_NO_DEPRECATED 必须对所有目标统一定义（见根 CMakeLists.txt）"
#endif

namespace llmgw {

/// 异步函数的返回类型，用法：`Task<int> f() { co_return 1; }`，调用方 `co_await f()`。
///
/// 约定（code-architecture.md 3.4）：
/// - 协程的参数不要用引用，按值传递或传智能指针
/// - 协程中不能阻塞，也不能在持有锁时 `co_await`
/// - 被取消时（客户端断开、超时），正在等待的操作以 `operation_aborted` 结束（使用 as_tuple 时
///   作为错误码返回），**协程会继续往下执行**；之后的下一次 `co_await` 才会抛出异常。
///   所以被取消之后、产生任何副作用（写响应、更新熔断计数、记录尝试）之前，必须先检查：
///   `(co_await asio::this_coro::cancellation_state).cancelled()`（任务 1.3 的教训）
/// - 参加 `a || b` 竞速的协程不能抛异常：`||` 是 wait_for_one_success，抛异常的分支不会结束竞速，
///   而是被悄悄吞掉、一直等到别的分支成功（独立审查发现）
template <class T = void>
using Task = boost::asio::awaitable<T>;

/// 检查当前协程是否已被取消（见上面的约定：被取消之后、产生任何副作用之前先检查）。用法：
///
///     if (is_cancelled(co_await kCancellationState)) co_return ...;
///
/// ⚠️ 不能写成一个返回 Task<bool> 的协程：被取消之后 co_await 另一个协程本身就会抛出
/// operation_aborted，检查永远返回不了 true（任务 1.7 发现：server 中原来的 is_cancelled()
/// 就是这样，四处检查都是靠抛异常“碰巧”结束的）。co_await this_coro::cancellation_state 不会抛出。
inline constexpr auto kCancellationState = boost::asio::this_coro::cancellation_state;

[[nodiscard]] inline bool is_cancelled(const boost::asio::cancellation_state& state) {
  return state.cancelled() != boost::asio::cancellation_type::none;
}

}  // namespace llmgw
