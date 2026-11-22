// 对单个 JSON（一个 SSE 事件的 data，或者整个非流式响应体）的检查：usage、业务错误码。
#pragma once

#include <optional>
#include <string_view>

#include "llmgw/core/request_context.h"
#include "llmgw/stream/stream.h"

namespace llmgw::stream::detail {

struct UsageFound {
  Usage usage;  ///< reported 一定为 true
  bool usage_only =
      false;  ///< choices 是空数组：这是“只有 usage 的块”（OpenAI 开启 include_usage 后的最后一块）
};

/// 从顶层的 usage 对象中取
/// prompt_tokens、completion_tokens、completion_tokens_details.reasoning_tokens。 没有 usage、usage
/// 是 null、不是合法的 JSON、字段类型不对或者为负数时返回空（不写入半个结果）。 缺少的字段按 0。
std::optional<UsageFound> extract_usage(std::string_view json);

/// 按 mapping.path 找到值（数字或字符串，按文本比较），返回映射后的状态码；没有命中时返回空。
std::optional<int> mapped_status(std::string_view json, const ErrorMapping& mapping);

}  // namespace llmgw::stream::detail
