#include "metrics.h"

#include <sstream>

namespace llmgw {

namespace {

// Prometheus 标签值需要转义反斜杠、双引号和换行
std::string esc(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  for (char c : s) {
    if (c == '\\' || c == '"') {
      out += '\\';
      out += c;
    } else if (c == '\n') {
      out += "\\n";
    } else {
      out += c;
    }
  }
  return out;
}

}  // namespace

void Metrics::record_request(const std::string& model, const std::string& provider, int status,
                             bool stream, double latency_ms) {
  std::lock_guard<std::mutex> lock(mu_);
  ++requests_[{model, provider, status, stream}];
  auto& h = latency_by_model_[model];
  for (size_t i = 0; i < kBucketsMs.size(); ++i) {
    if (latency_ms <= kBucketsMs[i]) ++h.buckets[i];
  }
  ++h.count;
  h.sum += latency_ms;
}

void Metrics::record_upstream_failure(const std::string& provider, const std::string& reason) {
  std::lock_guard<std::mutex> lock(mu_);
  ++upstream_failures_[{provider, reason}];
}

void Metrics::record_tokens(const std::string& model, const std::string& provider,
                            int64_t prompt_tokens, int64_t completion_tokens) {
  std::lock_guard<std::mutex> lock(mu_);
  auto& t = tokens_[{model, provider}];
  t.first += prompt_tokens;
  t.second += completion_tokens;
}

std::string Metrics::render() const {
  std::lock_guard<std::mutex> lock(mu_);
  std::ostringstream o;

  o << "# HELP llmgw_requests_total Client requests handled by the gateway.\n"
       "# TYPE llmgw_requests_total counter\n";
  for (const auto& [key, n] : requests_) {
    const auto& [model, provider, status, stream] = key;
    o << "llmgw_requests_total{model=\"" << esc(model) << "\",provider=\"" << esc(provider)
      << "\",status=\"" << status << "\",stream=\"" << (stream ? "true" : "false") << "\"} " << n
      << "\n";
  }

  o << "# HELP llmgw_request_duration_ms End-to-end request latency in milliseconds.\n"
       "# TYPE llmgw_request_duration_ms histogram\n";
  for (const auto& [model, h] : latency_by_model_) {
    for (size_t i = 0; i < kBucketsMs.size(); ++i) {
      o << "llmgw_request_duration_ms_bucket{model=\"" << esc(model) << "\",le=\""
        << kBucketsMs[i] << "\"} " << h.buckets[i] << "\n";
    }
    o << "llmgw_request_duration_ms_bucket{model=\"" << esc(model) << "\",le=\"+Inf\"} "
      << h.count << "\n";
    o << "llmgw_request_duration_ms_sum{model=\"" << esc(model) << "\"} " << h.sum << "\n";
    o << "llmgw_request_duration_ms_count{model=\"" << esc(model) << "\"} " << h.count << "\n";
  }

  o << "# HELP llmgw_upstream_failures_total Failed upstream attempts (each may trigger failover).\n"
       "# TYPE llmgw_upstream_failures_total counter\n";
  for (const auto& [key, n] : upstream_failures_) {
    o << "llmgw_upstream_failures_total{provider=\"" << esc(key.first) << "\",reason=\""
      << esc(key.second) << "\"} " << n << "\n";
  }

  o << "# HELP llmgw_tokens_total Tokens reported by upstream usage fields.\n"
       "# TYPE llmgw_tokens_total counter\n";
  for (const auto& [key, t] : tokens_) {
    o << "llmgw_tokens_total{model=\"" << esc(key.first) << "\",provider=\"" << esc(key.second)
      << "\",type=\"prompt\"} " << t.first << "\n";
    o << "llmgw_tokens_total{model=\"" << esc(key.first) << "\",provider=\"" << esc(key.second)
      << "\",type=\"completion\"} " << t.second << "\n";
  }
  return o.str();
}

}  // namespace llmgw
