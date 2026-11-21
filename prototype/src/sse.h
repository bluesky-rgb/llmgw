#pragma once

#include <cstdint>
#include <string>

namespace llmgw {

struct TokenUsage {
  int64_t prompt_tokens = 0;
  int64_t completion_tokens = 0;
  bool present = false;
};

// 从非流式响应体中读取 usage 字段
TokenUsage extract_usage_from_json(const std::string& body);

// 流式响应会被切成任意大小的块到达。这个类按行重新拼接 SSE 数据，
// 找到带 usage 的那一行（OpenAI 需要请求里带 stream_options.include_usage，
// llama.cpp 默认会在最后一块里带上）
class SseUsageScanner {
 public:
  void feed(const char* data, size_t len);
  const TokenUsage& usage() const { return usage_; }

 private:
  void handle_line(const std::string& line);

  std::string buffer_;
  TokenUsage usage_;
};

}  // namespace llmgw
