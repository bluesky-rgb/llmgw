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
/// S20）。 只检查到能取出这两个字段为止：请求体其余部分的合法性由 provider（任务 1.6）检查。
///
/// body 会被预留 simdjson 需要的末尾空间（capacity 增加 64 字节），内容不变。
/// 每个线程复用一个解析器（Worker 线程各自一个）。
Result<ChatFields> parse_chat_fields(std::string* body);

/// GET /v1/models 的响应体（OpenAI 格式），列出配置中的全部模型。
std::string models_body(const config::Snapshot& snapshot);

/// 去掉请求目标中的查询参数："/v1/models?x=1" → "/v1/models"。
std::string_view path_of(std::string_view target);

/// 逐跳（hop-by-hop）的响应头：转发上游的响应头时要去掉，由网关自己决定。
bool is_hop_by_hop(std::string_view header_name);

}  // namespace llmgw::server::detail
