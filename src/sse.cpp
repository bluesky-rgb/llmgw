#include "sse.h"

#include <nlohmann/json.hpp>

namespace llmgw {

namespace {

TokenUsage usage_from(const nlohmann::json& j) {
  TokenUsage u;
  if (j.is_object() && j.contains("usage") && j["usage"].is_object()) {
    const auto& usage = j["usage"];
    u.prompt_tokens = usage.value("prompt_tokens", int64_t{0});
    u.completion_tokens = usage.value("completion_tokens", int64_t{0});
    u.present = true;
  }
  return u;
}

}  // namespace

TokenUsage extract_usage_from_json(const std::string& body) {
  auto j = nlohmann::json::parse(body, nullptr, /*allow_exceptions=*/false);
  if (j.is_discarded()) return {};
  return usage_from(j);
}

void SseUsageScanner::feed(const char* data, size_t len) {
  buffer_.append(data, len);
  size_t start = 0;
  for (;;) {
    const auto nl = buffer_.find('\n', start);
    if (nl == std::string::npos) break;
    std::string line = buffer_.substr(start, nl - start);
    if (!line.empty() && line.back() == '\r') line.pop_back();
    handle_line(line);
    start = nl + 1;
  }
  buffer_.erase(0, start);
  // 防御：异常的上游如果一直不发换行，不能无限占用内存
  if (buffer_.size() > 1 << 20) buffer_.clear();
}

void SseUsageScanner::handle_line(const std::string& line) {
  static const std::string kPrefix = "data:";
  if (line.compare(0, kPrefix.size(), kPrefix) != 0) return;
  // 先做便宜的字符串检查，只有可能包含 usage 的行才做 JSON 解析
  if (line.find("\"usage\"") == std::string::npos) return;

  auto j = nlohmann::json::parse(line.substr(kPrefix.size()), nullptr, false);
  if (j.is_discarded()) return;
  auto u = usage_from(j);
  if (u.present) usage_ = u;
}

}  // namespace llmgw
