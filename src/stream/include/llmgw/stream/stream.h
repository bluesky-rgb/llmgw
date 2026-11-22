// 把上游的响应体包装成 core 的 ResponseStream（任务 1.5）。设计见 docs/tasks/1.5-stream.md。
#pragma once

#include <cstddef>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "llmgw/core/request_context.h"
#include "llmgw/core/response.h"
#include "llmgw/core/result.h"
#include "llmgw/core/task.h"

namespace llmgw::stream {

/// 字节来源。provider 把 upstream::Body 适配成它（stream 不依赖 upstream，任务设计第 2 节）。
/// 只在创建它的 Worker 线程上使用。
class Source {
 public:
  Source() = default;
  Source(const Source&) = delete;
  Source& operator=(const Source&) = delete;
  Source(Source&&) = delete;
  Source& operator=(Source&&) = delete;
  virtual ~Source() = default;

  /// 把下一段数据追加到 out（不清空 out）。true：读到了数据；false：正常结束，之后不要再调用；
  /// 错误：中途出错，之后不要再调用
  [[nodiscard]] virtual Task<Result<bool>> read(std::string* out) = 0;
};

/// 业务错误码映射（providers.md 3.4）：响应体中 path 处的值 → HTTP 状态码。
/// provider 从 config::ErrorMapping 转换（stream 不依赖 config）。
struct ErrorMapping {
  std::vector<std::string> path;  ///< 比如 {"base_resp", "status_code"}
  std::map<std::string, int>
      codes;  ///< 值的文本形式（数字和字符串一样处理）→ 状态码，比如 "1002" → 429
};

struct Options {
  /// 网关替客户端加上了 stream_options.include_usage：不转发“只有 usage 的块”（客户端没有要求它）
  bool strip_usage_chunk = false;
  /// 为空时不检查业务错误码；不为空时必须比返回的 ResponseStream 活得久（指向配置快照中的对象）
  const ErrorMapping* error_mapping = nullptr;
  std::size_t max_event_bytes = std::size_t{4} << 20;  ///< 单个 SSE 事件的上限
  std::size_t max_inspect_bytes = std::size_t{32}
                                  << 20;  ///< 非流式：为提取 usage、检查错误码保留的副本的上限
};

/// 包装上游的响应。
///
/// - 2xx 且 Content-Type 是 text/event-stream：按 SSE 解析，每一块是一个或多个完整的事件；
///   其他情况（包括流式请求得到了 JSON 错误体）：原样转发
/// - 提取 usage 和首字延迟，写入 ctx->usage、ctx->ttft
/// - 配置了 error_mapping 且状态码是 2xx 时，先读到能做判断为止（流式：第一个带 data 的事件；
///   非流式：整个响应体），命中时把状态码改成映射后的值。读的过程中出错时返回错误
///
/// head 是上游的状态码和响应头（原样交给 Session，Session 会去掉逐跳的头）。
/// ctx 必须比返回的 ResponseStream 活得久（Session 持有两者）。
[[nodiscard]] Task<Result<std::unique_ptr<ResponseStream>>> open(ResponseHead head,
                                                                 std::unique_ptr<Source> source,
                                                                 RequestContext* ctx,
                                                                 Options options);

}  // namespace llmgw::stream
