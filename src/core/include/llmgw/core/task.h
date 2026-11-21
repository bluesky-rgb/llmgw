// Task<T>：所有异步函数的返回类型。
//
// 这是 core 中唯一引用 Boost.Asio 的地方（ADR-007）。其他头文件只使用 Task，
// 不直接出现 boost::asio 的其他类型；将来如果更换协程库，只需修改这一个别名。
#pragma once

#include <boost/asio/awaitable.hpp>

namespace llmgw {

/// 异步函数的返回类型，用法：`Task<int> f() { co_return 1; }`，调用方 `co_await f()`。
///
/// 约定（code-architecture.md 3.4）：
/// - 协程的参数不要用引用，按值传递或传智能指针
/// - 协程中不能阻塞，也不能在持有锁时 `co_await`
/// - 被取消时（客户端断开、超时），正在等待的操作会以 `operation_aborted` 结束，
///   取消沿着 `co_await` 链自动传递，下层不需要检查任何标志
template <class T = void>
using Task = boost::asio::awaitable<T>;

}  // namespace llmgw
