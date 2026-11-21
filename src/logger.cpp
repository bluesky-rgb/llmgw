#include "logger.h"

#include <atomic>
#include <chrono>
#include <ctime>
#include <iostream>
#include <mutex>

namespace llmgw {

namespace {
std::mutex g_log_mu;
std::atomic<bool> g_enabled{true};

std::string now_iso8601() {
  using namespace std::chrono;
  const auto now = system_clock::now();
  const auto t = system_clock::to_time_t(now);
  const auto ms = duration_cast<milliseconds>(now.time_since_epoch()).count() % 1000;
  std::tm tm{};
  gmtime_r(&t, &tm);
  char buf[32];
  std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tm);
  char out[40];
  std::snprintf(out, sizeof(out), "%s.%03dZ", buf, static_cast<int>(ms));
  return out;
}
}  // namespace

void set_logging_enabled(bool enabled) { g_enabled = enabled; }

void log_event(const std::string& level, const std::string& msg, nlohmann::json fields) {
  if (!g_enabled) return;
  fields["ts"] = now_iso8601();
  fields["level"] = level;
  fields["msg"] = msg;
  // replace 模式：上游返回的非法 UTF-8 不会让日志抛异常
  const auto line = fields.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
  std::lock_guard<std::mutex> lock(g_log_mu);
  std::cout << line << '\n' << std::flush;
}

}  // namespace llmgw
