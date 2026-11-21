#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <tuple>

namespace llmgw {

// 线程安全的指标收集，按 Prometheus 文本格式输出（GET /metrics）
class Metrics {
 public:
  // 一次客户端请求结束时调用
  void record_request(const std::string& model, const std::string& provider, int status,
                      bool stream, double latency_ms);
  // 某个上游尝试失败（连接失败、5xx、429），触发了切换或最终失败
  void record_upstream_failure(const std::string& provider, const std::string& reason);
  void record_tokens(const std::string& model, const std::string& provider,
                     int64_t prompt_tokens, int64_t completion_tokens);

  std::string render() const;

 private:
  static constexpr std::array<double, 11> kBucketsMs = {
      10, 50, 100, 250, 500, 1000, 2500, 5000, 10000, 30000, 60000};

  struct Histogram {
    std::array<uint64_t, kBucketsMs.size()> buckets{};
    uint64_t count = 0;
    double sum = 0;
  };

  mutable std::mutex mu_;
  // (model, provider, status, stream) -> count
  std::map<std::tuple<std::string, std::string, int, bool>, uint64_t> requests_;
  std::map<std::string, Histogram> latency_by_model_;
  std::map<std::pair<std::string, std::string>, uint64_t> upstream_failures_;
  std::map<std::pair<std::string, std::string>, std::pair<int64_t, int64_t>> tokens_;
};

}  // namespace llmgw
