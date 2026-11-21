// 内部：Session，每条客户端连接一个协程。
//
//   session.cpp —— Keep-Alive 循环、读请求（大小上限与读取超时）、路由、错误响应
//   relay.cpp   —— /v1/chat/completions：一次竞速 + 拉取循环（ADR-006）
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <random>
#include <string>

#include "async_util.h"
#include "llmgw/config/snapshot.h"
#include "llmgw/core/error.h"
#include "llmgw/core/metrics.h"
#include "llmgw/server/dispatcher.h"

namespace llmgw::server::detail {

/// 所有 Worker 共享的状态。只有这两个原子变量是跨线程写的。
struct SharedState {
  std::atomic<std::int64_t> connections{0};  ///< 当前的数据面连接数（max_connections 的计数）
  std::atomic<bool> stopping{false};  ///< 正在停止：/ready 返回 503
};

/// 一个 Worker 上所有 Session 共用的资源。由 Worker 持有，生命周期长于它的所有 Session。
struct WorkerContext {
  unsigned index = 0;
  std::shared_ptr<const config::Snapshot> config;
  Dispatcher* dispatcher = nullptr;
  MetricsRecorder* metrics = nullptr;
  SharedState* shared = nullptr;
  std::mt19937_64 rng;  ///< 生成请求 ID；只在本 Worker 的线程上使用
};

/// 连接计数的 RAII：Session 无论怎样结束（包括异常），计数都会减少。
class ConnectionGuard {
 public:
  explicit ConnectionGuard(SharedState* s) : shared_(s) {}
  ConnectionGuard(const ConnectionGuard&) = delete;
  ConnectionGuard& operator=(const ConnectionGuard&) = delete;
  ConnectionGuard(ConnectionGuard&&) = delete;
  ConnectionGuard& operator=(ConnectionGuard&&) = delete;
  ~ConnectionGuard() { shared_->connections.fetch_sub(1, std::memory_order_relaxed); }

 private:
  SharedState* shared_;
};

using Request = http::request<http::string_body>;

class Session {
 public:
  /// 调用者已经把连接计入 shared->connections；guard 负责在结束时减去。
  Session(tcp::socket socket, WorkerContext* worker);

  /// Keep-Alive 循环：依次处理同一条连接上的请求，直到连接关闭或不能复用。
  Task<> run();

 private:
  enum class ReadResult : std::uint8_t { kOk, kClose };
  // 读一个完整的请求。出错时已经回复了错误（或者按规则不回复），返回 kClose。
  Task<ReadResult> read_request(std::optional<Request>* out);
  Task<beast::error_code> read_body(http::request_parser<http::string_body>* parser,
                                    SteadyClock::time_point* last_activity);
  // 处理一个请求，返回连接能否继续复用
  Task<bool> handle(Request request);
  Task<bool> handle_chat(Request request, std::string request_id);

  // ---- relay.cpp ----
  struct Exchange;
  enum class PumpResult : std::uint8_t { kDone, kStreamError, kClientError };
  Task<bool> relay(Exchange* ex);
  Task<PumpResult> pump(Exchange* ex, ResponseStream* stream);
  Task<bool> write_head(Exchange* ex, const ResponseHead* head);
  // data 必须在 co_await 期间有效：调用方总是立即 co_await，数据在调用方的栈帧里
  Task<bool> write_chunk(const Exchange* ex, std::string_view data);
  Task<bool> finish_body(const Exchange* ex);

  // ---- 响应 ----
  /// 写一个完整的响应（JSON），返回是否写成功。
  // 协程参数一律按值传递（code-architecture.md 3.4）
  Task<bool> write_json(unsigned status, std::string body, std::string request_id, bool keep_alive,
                        unsigned version, std::optional<Error> error = std::nullopt);
  Task<bool> write_error(Error error, std::string request_id, bool keep_alive, unsigned version);
  /// 回复错误后关闭连接：先关闭写方向，再丢弃对方还在发送的数据一小段时间，
  /// 避免直接关闭导致内核发送 RST、客户端收不到我们的回复（比如 413 时对方还在上传请求体）。
  Task<> reject_and_close(Error error, std::string reason_label);

  tcp::socket socket_;
  beast::flat_buffer buffer_;  ///< 跨请求保留：管线化时可能已经读到了下一个请求的开头
  WorkerContext* worker_;
  ConnectionGuard guard_;
};

/// 在 accept 之后立即拒绝（连接数超过上限）：回复 503 并关闭。
Task<> reject_overloaded(tcp::socket socket, WorkerContext* worker);

/// 运行一个 Session（co_spawn 的入口；参数按值传入，所有权转移给协程）。
Task<> run_session(std::unique_ptr<Session> session);

}  // namespace llmgw::server::detail
