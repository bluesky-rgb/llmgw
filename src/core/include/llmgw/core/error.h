// 网关自己产生的错误：错误码、HTTP 状态码、响应体中的 code、是否建议客户端重试。
//
// 与 docs/client-guide.md 第 7 节的表格一一对应，单元测试逐行核对。
// 上游返回的 4xx（比如参数错误）按 ADR-002 原样透传，不转换成 Error。
#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace llmgw {

/// 错误码。新增时必须同时更新下面的三个映射函数（switch 没有 default，漏写会编译失败）
/// 和 client-guide.md 第 7 节。
enum class ErrorCode : std::uint8_t {
  kInvalidRequest,       ///< 400 请求参数错误
  kInvalidApiKey,        ///< 401 网关 key 无效
  kBudgetExceeded,       ///< 402 预算用完
  kModelNotFound,        ///< 404 模型名不存在
  kNotFound,             ///< 404 路径不存在
  kMethodNotAllowed,     ///< 405 方法不对（比如 GET /v1/chat/completions）
  kPayloadTooLarge,      ///< 413 请求太大
  kRateLimited,          ///< 429 超过限流额度
  kHeadersTooLarge,      ///< 431 请求头太大
  kInternal,             ///< 500 网关自身的错误
  kUpstreamUnavailable,  ///< 502 所有上游都不可用
  kOverloaded,           ///< 503 网关过载
  kUpstreamTimeout,      ///< 504 上游超时（请求已发出，可能已经计费）
};

/// 返回给客户端的 HTTP 状态码。
constexpr int http_status(ErrorCode code) {
  switch (code) {
    case ErrorCode::kInvalidRequest:
      return 400;
    case ErrorCode::kInvalidApiKey:
      return 401;
    case ErrorCode::kBudgetExceeded:
      return 402;
    case ErrorCode::kModelNotFound:
    case ErrorCode::kNotFound:
      return 404;
    case ErrorCode::kMethodNotAllowed:
      return 405;
    case ErrorCode::kPayloadTooLarge:
      return 413;
    case ErrorCode::kRateLimited:
      return 429;
    case ErrorCode::kHeadersTooLarge:
      return 431;
    case ErrorCode::kInternal:
      return 500;
    case ErrorCode::kUpstreamUnavailable:
      return 502;
    case ErrorCode::kOverloaded:
      return 503;
    case ErrorCode::kUpstreamTimeout:
      return 504;
  }
  return 500;  // 不会到达：上面覆盖了所有枚举值
}

/// 响应体 `{"error": {"code": ...}}` 中的错误码字符串。
constexpr std::string_view code_name(ErrorCode code) {
  switch (code) {
    case ErrorCode::kInvalidRequest:
      return "invalid_request";
    case ErrorCode::kInvalidApiKey:
      return "invalid_api_key";
    case ErrorCode::kBudgetExceeded:
      return "budget_exceeded";
    case ErrorCode::kModelNotFound:
      return "model_not_found";
    case ErrorCode::kNotFound:
      return "not_found";
    case ErrorCode::kMethodNotAllowed:
      return "method_not_allowed";
    case ErrorCode::kPayloadTooLarge:
      return "payload_too_large";
    case ErrorCode::kRateLimited:
      return "rate_limited";
    case ErrorCode::kHeadersTooLarge:
      return "headers_too_large";
    case ErrorCode::kInternal:
      return "internal_error";
    case ErrorCode::kUpstreamUnavailable:
      return "upstream_unavailable";
    case ErrorCode::kOverloaded:
      return "overloaded";
    case ErrorCode::kUpstreamTimeout:
      return "upstream_timeout";
  }
  return "internal_error";  // 不会到达
}

/// 响应头 `x-should-retry` 的取值：是否建议客户端自动重试。
///
/// 504 和 500 返回 false：请求可能已经发到上游并开始计费，自动重试可能付两次钱。
constexpr bool should_retry(ErrorCode code) {
  switch (code) {
    case ErrorCode::kRateLimited:
    case ErrorCode::kUpstreamUnavailable:
    case ErrorCode::kOverloaded:
      return true;
    case ErrorCode::kInvalidRequest:
    case ErrorCode::kInvalidApiKey:
    case ErrorCode::kBudgetExceeded:
    case ErrorCode::kModelNotFound:
    case ErrorCode::kNotFound:
    case ErrorCode::kMethodNotAllowed:
    case ErrorCode::kPayloadTooLarge:
    case ErrorCode::kHeadersTooLarge:
    case ErrorCode::kInternal:
    case ErrorCode::kUpstreamTimeout:
      return false;
  }
  return false;  // 不会到达
}

/// 响应体 `{"error": {"type": ...}}` 中的错误类型（与原型 v0.1、OpenAI 的写法一致）。
constexpr std::string_view error_type(ErrorCode code) {
  switch (code) {
    case ErrorCode::kInvalidRequest:
    case ErrorCode::kBudgetExceeded:
    case ErrorCode::kModelNotFound:
    case ErrorCode::kNotFound:
    case ErrorCode::kMethodNotAllowed:
    case ErrorCode::kPayloadTooLarge:
    case ErrorCode::kHeadersTooLarge:
      return "invalid_request_error";
    case ErrorCode::kInvalidApiKey:
      return "authentication_error";
    case ErrorCode::kRateLimited:
      return "rate_limit_error";
    case ErrorCode::kInternal:
    case ErrorCode::kOverloaded:
      return "server_error";
    case ErrorCode::kUpstreamUnavailable:
    case ErrorCode::kUpstreamTimeout:
      return "upstream_error";
  }
  return "server_error";  // 不会到达
}

/// 网关自己产生的一个错误。
struct Error {
  ErrorCode code = ErrorCode::kInternal;
  /// 给人看的说明，会出现在响应体中：不能包含 key、上游地址等敏感信息。
  std::string message;
  /// 建议客户端多久之后重试（`Retry-After`）；没有值表示不给出建议。
  std::optional<std::chrono::seconds> retry_after;
};

}  // namespace llmgw
