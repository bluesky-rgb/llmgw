// 模糊测试：SSE 解析器（任务 1.5 S10）。
// 输入的第一个字节作为切块的随机种子，其余是上游发来的字节。不变式：
// 不崩溃；按任意方式切块喂入，与一次性喂入的结果（事件、原始字节、能否正常结束）完全相同；
// 转发的字节是输入的前缀（加上结束时可能补上的空行），不会多出、改动或者重复任何字节。
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "inspect.h"
#include "llmgw/stream/sse_parser.h"

namespace {

using llmgw::stream::SseEvent;
using llmgw::stream::SseParser;

struct Result {
  std::vector<SseEvent> events;
  bool ok = true;
  bool overflowed = false;
};

Result run(std::string_view text, std::uint32_t seed, std::size_t limit) {
  SseParser p(limit);
  Result r;
  std::size_t pos = 0;
  while (pos < text.size()) {
    std::size_t n = text.size() - pos;
    if (seed != 0) {
      seed = seed * 1103515245U + 12345U;
      n = std::min<std::size_t>(n, 1 + (seed >> 16U) % 17);
    }
    p.feed(text.substr(pos, n));
    pos += n;
    while (auto e = p.next()) r.events.push_back(std::move(*e));
  }
  r.overflowed = p.overflowed();
  r.ok = p.finish();
  while (auto e = p.next()) r.events.push_back(std::move(*e));
  return r;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  if (size == 0) return 0;
  const std::uint32_t seed = data[0] | 1U;  // 非零：一定按小块切
  const std::string_view text(reinterpret_cast<const char*>(data) + 1, size - 1);
  // 上限由输入决定（16–79 字节）：上限的边界情况（比如结束时补上空行恰好超过上限）才能被经常走到。
  // 固定为 4096 时，反向验证发现 60 秒内一次都没有走到
  const std::size_t limit = 16 + (data[0] >> 2U) % 64;
  const Result whole = run(text, 0, limit);
  const Result split = run(text, seed, limit);
  if (whole.ok != split.ok || whole.overflowed != split.overflowed) __builtin_trap();
  if (whole.events.size() != split.events.size()) __builtin_trap();
  std::string forwarded;
  for (std::size_t i = 0; i < whole.events.size(); ++i) {
    const SseEvent& a = whole.events[i];
    const SseEvent& b = split.events[i];
    if (a.raw != b.raw || a.data != b.data || a.event != b.event || a.has_data != b.has_data)
      __builtin_trap();
    forwarded += a.raw;
  }
  // 转发的字节：输入的前缀，最后一个事件可能补上了一个空行（1 或 2 个字节）
  const std::size_t common = std::min(forwarded.size(), text.size());
  if (forwarded.substr(0, common) != text.substr(0, common)) __builtin_trap();
  if (forwarded.size() > text.size() + 2) __builtin_trap();
  // 正常结束时，一个字节都不能少：转发的就是输入本身，或者输入加上补上的空行（独立审查 #2、#11）
  if (whole.ok) {
    const bool same = forwarded == text;
    const bool completed =
        forwarded.size() > text.size() && forwarded.starts_with(text) &&
        (forwarded.substr(text.size()) == "\n" || forwarded.substr(text.size()) == "\r" ||
         forwarded.substr(text.size()) == "\r\n");
    if (!same && !completed) __builtin_trap();
  }
  // 对每个事件的 data 做 usage 提取和错误码检查：任意输入都不能让 simdjson 出问题
  static const llmgw::stream::ErrorMapping kMapping{.path = {"base_resp", "status_code"},
                                                    .codes = {{"1002", 429}}};
  for (const auto& e : whole.events) {
    if (auto u = llmgw::stream::detail::extract_usage(e.data)) {
      if (u->usage.prompt_tokens < 0 || u->usage.completion_tokens < 0 || !u->usage.reported)
        __builtin_trap();
    }
    if (auto s = llmgw::stream::detail::mapped_status(e.data, kMapping); s && *s != 429)
      __builtin_trap();
  }
  return 0;
}
