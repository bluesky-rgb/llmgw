#pragma once

#include <string>

#include <nlohmann/json.hpp>

namespace llmgw {

// 每条日志输出一行 JSON，方便用 jq、Loki、ELK 等工具检索
void log_event(const std::string& level, const std::string& msg,
               nlohmann::json fields = nlohmann::json::object());

// 关闭日志（测试时使用）
void set_logging_enabled(bool enabled);

}  // namespace llmgw
