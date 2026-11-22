// 内部：与 I/O 无关的 HTTP 辅助函数（错误响应、请求
// ID、请求体字段、模型列表）。都是纯函数，单独测试。
#pragma once

#include <cstdint>
#include <random>
#include <string>
#include <string_view>

#include "llmgw/config/snapshot.h"
#include "llmgw/core/error.h"
#include "llmgw/core/result.h"

namespace llmgw::server::detail {

/// 请求头的上限。超过返回 431。
inline constexpr std::uint32_t kMaxHeaderBytes = 64 * 1024;

/// OpenAI 格式的错误响应体：{"error": {"message", "type", "code"}}。
std::string error_body(const Error& error);

/// 流式响应中途出错时发送的 SSE 事件："data: {"error": …}\n\n"。
std::string error_event(const Error& error);

/// 客户端给的 X-Request-Id 合法（长度 1–64，只包含字母数字和 ._-）就沿用，否则生成一个。
std::string request_id(std::string_view from_client, std::mt19937_64& rng);

/// 从 /v1/chat/completions 的请求体中取出的字段。
struct ChatFields {
  std::string model;
  bool stream = false;
};

/// 请求体必须是 JSON 对象；model 是非空字符串；stream 可以没有，有的话必须是布尔值。
///
/// 在热路径上：用 simdjson 的 on-demand 模式只读顶层的这两个字段，不构建整棵树
/// （1 MB 的请求体约 75 µs；nlohmann 需要 7.5 ms，期间整个 Worker 的连接都会停住，见任务 1.3 的
/// S20）。 会拒绝重复的 model / stream、对象之后多余的内容；不会检查跳过的值内部是否合法（on-demand
/// 模式不展开它们），这部分由 provider（任务 1.6）解析整个请求体时检查。
///
/// body 会被预留 simdjson 需要的末尾空间（capacity 增加 64 字节），内容不变。
/// 每个线程复用一个解析器（Worker 线程各自一个）。
Result<ChatFields> parse_chat_fields(std::string* body);

/// GET /v1/models 的响应体（OpenAI 格式），列出配置中的全部模型。
std::string models_body(const config::Snapshot& snapshot);

/// 请求目标的路径部分：去掉查询参数；绝对形式（RFC 9112 3.2.2，服务端必须接受）去掉协议和主机。
/// "/v1/models?x=1" → "/v1/models"；"http://host/v1/models" → "/v1/models"。
std::string_view path_of(std::string_view target);

/// 响应头能否安全地写给客户端：名字只包含 RFC 9110 的 token 字符，值中没有 CR、LF、NUL 等控制字符
/// （允许水平制表符）。用于转发上游的响应头：不合法的直接丢弃，否则可以向客户端注入任意响应头
/// （Boost.Beast 的 insert() 不做这项检查）。
bool safe_header(std::string_view name, std::string_view value);

/// Transfer-Encoding 是否恰好是 "chunked"（忽略大小写和首尾空白）。其他任何编码都要拒绝：
/// 网关不支持 gzip 等编码，“chunked 不是最后一个”会让请求体在哪里结束有歧义（请求走私）。
bool is_plain_chunked(std::string_view transfer_encoding);

/// 逐跳（hop-by-hop）的响应头：转发上游的响应头时要去掉，由网关自己决定。
bool is_hop_by_hop(std::string_view header_name);

/// Authorization 头中的 Bearer token（RFC 6750 2.1：`Bearer` 大小写不敏感，后面一个或多个空格，
/// 然后是 token，token 中不能有空白）。不是这种格式时返回空。
std::string bearer_token(std::string_view authorization);

/// 中间件附加的响应头能否写出：必须是合法的响应头（safe_header），不能是逐跳的头，也不能是网关自己
/// 决定的头（Content-Type、X-Request-Id、x-should-retry、Retry-After 等：限流插件要给出 Retry-After
/// 时用 Error::retry_after）。
bool plugin_header_allowed(std::string_view name, std::string_view value);

}  // namespace llmgw::server::detail
