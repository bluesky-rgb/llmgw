// 重试规则表（design.md 6.1，任务 1.7 第 4 节）：一次尝试的结果 → 分类 + 返回 / 换下一个。
// 纯函数，单元测试逐行覆盖 6.1 的表格。
#pragma once

#include <cstdint>

#include "llmgw/core/request_context.h"
#include "llmgw/upstream/client.h"

namespace llmgw::executor::detail {

enum class Next : std::uint8_t {
  kReturn,   ///< 把这个结果交给客户端
  kTryNext,  ///< 换下一个目标
};

struct Verdict {
  AttemptResult result;  ///< ctx->attempts 和指标中的分类
  Next next;
  /// 上游可能已经收到并处理（计费）了这个请求：已发出之后超时 / 断开、500、504 等
  bool ambiguous;

  friend bool operator==(const Verdict&, const Verdict&) = default;
};

/// 没有拿到响应头。retry_ambiguous：配置的 retry.retry_on_ambiguous
/// （最后一个目标没有下一个可换，调用方传 false）。
[[nodiscard]] Verdict classify(const upstream::AttemptError& e, bool retry_ambiguous);

/// 拿到了响应头（包括 error_mapping 映射之后的状态码）。
[[nodiscard]] Verdict classify(int status, bool retry_ambiguous);

/// 401、403：网关自己的上游密钥有问题（计入 llmgw_upstream_auth_errors_total）。
[[nodiscard]] constexpr bool is_auth_error(int status) { return status == 401 || status == 403; }

/// 所有目标都失败时，最后一次尝试的响应可以原样交给客户端（第 4 节规则 1）：
/// 429、502、503 带着上游的 Retry-After 等信息。401 / 403 / 402 / 3xx 不转发。
[[nodiscard]] constexpr bool forward_when_exhausted(int status) {
  return status == 429 || status == 502 || status == 503;
}

}  // namespace llmgw::executor::detail
