// OpenAI 兼容的上游适配（任务 1.6）：请求改写，以及一次尝试（upstream 发送 → stream 包装）。
// 设计见 docs/tasks/1.6-provider.md。
#pragma once

#include <memory>

#include "llmgw/config/snapshot.h"
#include "llmgw/core/request_context.h"
#include "llmgw/core/response.h"
#include "llmgw/core/result.h"
#include "llmgw/core/task.h"
#include "llmgw/upstream/client.h"

namespace llmgw::provider {

/// 一个目标的请求（prepare() 的结果）。
struct Prepared {
  upstream::Request request;
  bool stream = false;  ///< 客户端的请求是否流式（executor 据此选择 first_byte 的超时）
  bool strip_usage_chunk = false;  ///< 网关替客户端加上了 include_usage：stream 不转发 usage 块
};

/// 为一个目标准备请求。纯计算，不做 I/O。
///
/// 请求体：model 换成目标的模型名；流式请求加上 stream_options.include_usage；
/// 再依次执行目标的 drop_fields、rename_fields、set_fields（任务设计第 4 节）。
/// 请求头：Authorization: Bearer <密钥>、Content-Type、Accept、User-Agent。
/// 路径：base_path + "/chat/completions"。
///
/// 错误：请求体不是 JSON 对象（400）。这是客户端的错误，换目标也没有用，不重试
[[nodiscard]] Result<Prepared> prepare(const RequestInfo& request, const config::Provider& provider,
                                       const config::TargetConfig& target);

/// 一次尝试：发送、过滤上游的响应头（白名单）、包装成 ResponseStream。
///
/// 成功时 ResponseStream 的状态码可能是 4xx/5xx，由 executor 按状态码决定是否重试；
/// 失败时返回 upstream 的 AttemptError（阶段、maybe_sent）。不抛异常。
/// provider 必须指向 client 的配置中的上游；ctx 必须比返回的 ResponseStream 活得久。
/// prefetch_idle：配置了 error_mapping 时，返回之前要预读响应体（检查业务错误码），预读中每次
/// 读取最多等这么久，超时按 {kBody, maybe_sent, timed_out} 返回。传目标的 timeouts.idle
/// （不超过剩余的总时间）。返回之后的读取不受它限制（由 Session 的看门狗负责）。
[[nodiscard]] Task<Expected<std::unique_ptr<ResponseStream>, upstream::AttemptError>> attempt(
    upstream::Client* client, const config::Provider* provider, Prepared prepared,
    upstream::Deadlines deadlines, SteadyClock::duration prefetch_idle, RequestContext* ctx);

/// 上游的响应头中保留哪些（任务设计第 4
/// 节）：Content-Type、Cache-Control、Retry-After、retry-after-ms； 上游的请求 ID 改名为
/// X-LLMGW-Upstream-Request-Id。其他一律丢弃（openai-organization 会泄露网关账号的信息， 上游的
/// x-ratelimit-* 是网关账号的额度，与网关返回给客户端的同名头冲突）
[[nodiscard]] std::vector<Header> filter_response_headers(
    const std::vector<Header>& upstream_headers);

}  // namespace llmgw::provider
