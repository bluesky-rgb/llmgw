// 响应：ResponseHead、Chunk，以及 Session 拉取响应体的接口 ResponseStream。
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "llmgw/core/result.h"
#include "llmgw/core/task.h"

namespace llmgw {

/// 一个 HTTP 响应头字段。
struct Header {
  std::string name;
  std::string value;

  friend bool operator==(const Header&, const Header&) = default;
};

/// 响应的状态码和响应头。
struct ResponseHead {
  int status = 200;
  std::vector<Header> headers;
};

/// 响应体的一块数据，原样写给客户端。
///
/// 流式响应中，一块是一个或多个完整的 SSE 事件（`data: ...\n\n`），
/// 不会把一个事件切成两半；非流式响应中，一块是响应体的一段。
struct Chunk {
  std::string data;
};

/// 响应体的拉取接口（design.md 3.2“调用机制与数据流”）。
///
/// Session 循环调用 `co_await next()`，写完上一块才拉下一块，背压是自然的结果；
/// 下层从不反过来调用 Session。实现由 stream 模块（以及以后的缓存插件等）提供。
///
/// 线程：只在创建它的 Worker 线程上使用。
/// 生命周期：由 Session 以 `std::unique_ptr` 持有；析构时归还或关闭上游连接。
class ResponseStream {
 public:
  ResponseStream() = default;
  ResponseStream(const ResponseStream&) = delete;
  ResponseStream& operator=(const ResponseStream&) = delete;
  ResponseStream(ResponseStream&&) = delete;
  ResponseStream& operator=(ResponseStream&&) = delete;
  virtual ~ResponseStream() = default;

  /// 状态码和响应头。创建 ResponseStream 时已经拿到，任何时候都可以调用。
  [[nodiscard]] virtual const ResponseHead& head() const = 0;

  /// 拉取下一块数据。返回值有三种情况：
  /// - 有值且不为空：一块数据
  /// - 有值但为空（`std::nullopt`）：响应正常结束，之后不要再调用
  /// - 错误：上游中途出错（比如连接断开），之后不要再调用
  ///
  /// 上游中途断开是常见情况而不是异常，所以用返回值表示（热路径不抛异常）。
  /// 被取消时（客户端断开、超时），正在等待的读取以 operation_aborted 结束；实现可以返回一个错误，
  /// 调用方会先检查取消状态再决定怎么处理（见 task.h）。
  [[nodiscard]] virtual Task<Result<std::optional<Chunk>>> next() = 0;
};

}  // namespace llmgw
