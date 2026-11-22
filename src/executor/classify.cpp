// 重试规则表。每个分支注明对应 design.md 6.1 的哪一行。
#include "classify.h"

namespace llmgw::executor::detail {

Verdict classify(const upstream::AttemptError& e, bool retry_ambiguous) {
  const AttemptResult result = e.timed_out ? AttemptResult::kTimeout : AttemptResult::kConnectError;
  if (!e.maybe_sent) {
    // “DNS、连接、TLS 失败”“复用的连接在发送阶段失败”（upstream 已经换新连接重发过一次），
    // 以及连接池排队超时：请求没有到达上游，不可能计费
    const AttemptResult r = e.stage == upstream::Stage::kQueue ? AttemptResult::kTimeout : result;
    return {.result = r, .next = Next::kTryNext, .ambiguous = false};
  }
  // “读取超时、连接被重置（已发出请求，未收到响应头）”：结果不确定，默认返回 504
  return {.result = result,
          .next = retry_ambiguous ? Next::kTryNext : Next::kReturn,
          .ambiguous = true};
}

Verdict classify(int status, bool retry_ambiguous) {
  if (status >= 200 && status < 300) {
    return {.result = AttemptResult::kOk, .next = Next::kReturn, .ambiguous = false};
  }
  if (status == 429) {  // “429、502、503”
    return {.result = AttemptResult::kStatus429, .next = Next::kTryNext, .ambiguous = false};
  }
  if (status == 502 || status == 503) {
    return {.result = AttemptResult::kStatus5xx, .next = Next::kTryNext, .ambiguous = false};
  }
  if (status >= 500) {
    // “500、504”，以及其他 5xx（任务设计偏离 3）：上游可能已经处理，默认原样返回
    return {.result = AttemptResult::kStatus5xx,
            .next = retry_ambiguous ? Next::kTryNext : Next::kReturn,
            .ambiguous = true};
  }
  if (is_auth_error(status) || status == 402) {
    // “401、403、余额不足”：网关自己的上游密钥或账户有问题，换下一个（不转发给客户端）
    return {.result = AttemptResult::kStatus4xx, .next = Next::kTryNext, .ambiguous = false};
  }
  if (status >= 400) {  // “其他 4xx”：请求本身的问题，换上游也没用
    return {.result = AttemptResult::kStatus4xx, .next = Next::kReturn, .ambiguous = false};
  }
  // 1xx（upstream 已经跳过）、3xx：API 返回重定向通常是 base_url 写错了（任务设计偏离 4）
  return {.result = AttemptResult::kStatus5xx, .next = Next::kTryNext, .ambiguous = false};
}

}  // namespace llmgw::executor::detail
