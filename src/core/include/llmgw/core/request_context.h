// RequestContext：一个请求在各模块之间传递的全部数据。
//
// 规则（design.md 3.2“信息共享”）：
// - 只属于处理这个请求的协程，不需要加锁
// - 每组字段只有一个写入者，写在字段旁边；Session 创建时确定的字段是 const 成员，
//   编译器保证之后没有人能修改
// - 不放“取消信号”：取消由 Asio 沿 co_await 链自动传递（见 task.h）；
//   下层只需要 deadline 来计算每次尝试还剩多少时间
#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "llmgw/core/error.h"
#include "llmgw/core/target.h"

namespace llmgw {

// 配置快照的类型定义在 config 模块，而 config 依赖 core，所以这里只做前置声明。
// 这是 core 中唯一一处引用上层模块的名字：core 自己从不访问快照的内容，
// 需要读配置的模块（router、middleware 等）本来就依赖 config。
namespace config {
struct Snapshot;
}  // namespace config

using SteadyClock = std::chrono::steady_clock;

/// 请求本身的信息。写入者：Session（创建上下文时），之后只读。
struct RequestInfo {
  std::string request_id;               ///< 返回给客户端的 X-Request-Id
  std::string model;                    ///< 客户端请求的模型名（路由之前）
  bool stream = false;                  ///< 是否为流式请求
  std::string body;                     ///< 原始请求体
  SteadyClock::time_point received_at;  ///< 收到请求的时间
  SteadyClock::time_point deadline;     ///< 整个请求的截止时间
};

/// 鉴权结果。写入者：auth 中间件。
struct AuthInfo {
  std::string key_id;  ///< 网关 key 的标识（不是 key 本身）；空表示还没有通过鉴权
};

/// 一次上游尝试的结果分类，同时用作指标 `llmgw_upstream_attempts_total` 的 result 标签。
enum class AttemptResult : std::uint8_t {
  kOk,            ///< 拿到了 2xx 响应头
  kConnectError,  ///< 连接失败（拒绝、DNS、TLS）
  kTimeout,       ///< 连接、首字节或读取超时
  kStatus429,     ///< 上游限流
  kStatus4xx,     ///< 上游返回其他 4xx
  kStatus5xx,     ///< 上游返回 5xx
  kBreakerOpen,   ///< 熔断器打开，没有真正发出请求
  kCancelled,     ///< 客户端断开或总超时，尝试被取消
};

/// 指标标签的取值。
constexpr std::string_view label(AttemptResult r) {
  switch (r) {
    case AttemptResult::kOk:
      return "ok";
    case AttemptResult::kConnectError:
      return "conn_error";
    case AttemptResult::kTimeout:
      return "timeout";
    case AttemptResult::kStatus429:
      return "429";
    case AttemptResult::kStatus4xx:
      return "4xx";
    case AttemptResult::kStatus5xx:
      return "5xx";
    case AttemptResult::kBreakerOpen:
      return "breaker_open";
    case AttemptResult::kCancelled:
      return "cancelled";
  }
  return "unknown";  // 不会到达
}

/// 一次上游尝试的记录。写入者：executor（每尝试一次追加一条）。
struct Attempt {
  Target target;
  AttemptResult result = AttemptResult::kOk;
  int upstream_status = 0;          ///< 上游的 HTTP 状态码；0 表示没有收到响应头
  SteadyClock::duration elapsed{};  ///< 这次尝试的耗时
};

/// 用量。写入者：stream（从响应中提取）。
struct Usage {
  std::int64_t prompt_tokens = 0;
  std::int64_t completion_tokens = 0;
  std::int64_t reasoning_tokens = 0;
  bool reported = false;  ///< 上游是否给出了 usage；false 时由预算模块按估算结算
};

/// 请求的终态分类。
enum class OutcomeKind : std::uint8_t {
  kPending,        ///< 还没有结束
  kSuccess,        ///< 完整返回了响应
  kRejected,       ///< 被中间件拒绝（401、429、402 …）
  kFailed,         ///< 网关或上游错误，没有返回完整结果
  kClientAborted,  ///< 客户端在完成之前断开
  kTimeout,        ///< 总超时或空闲超时
};

/// 请求的终态。写入者：Session；`on_complete` 中的所有中间件读取。
struct Outcome {
  OutcomeKind kind = OutcomeKind::kPending;
  int http_status = 0;  ///< 返回给客户端的状态码；0 表示还没有发出响应头
  std::optional<ErrorCode> error;  ///< 网关自己产生的错误（如果有）
};

/// 一个请求的上下文。只能移动，不能复制：一个请求只有一份上下文。
struct RequestContext {
  RequestContext(RequestInfo info, std::shared_ptr<const config::Snapshot> snapshot)
      : request(std::move(info)), config(std::move(snapshot)) {}
  RequestContext(const RequestContext&) = delete;
  RequestContext& operator=(const RequestContext&) = delete;
  RequestContext(RequestContext&&) = default;
  RequestContext& operator=(RequestContext&&) = delete;  // const 成员不能重新赋值
  ~RequestContext() = default;

  const RequestInfo request;                             ///< 写入者：Session（创建时）
  const std::shared_ptr<const config::Snapshot> config;  ///< 写入者：Session（创建时）
  AuthInfo auth;                                         ///< 写入者：auth 中间件
  std::vector<Attempt> attempts;                         ///< 写入者：executor
  Usage usage;                                           ///< 写入者：stream
  std::optional<SteadyClock::duration> ttft;             ///< 首字延迟。写入者：stream
  Outcome outcome;                                       ///< 写入者：Session
};

}  // namespace llmgw
