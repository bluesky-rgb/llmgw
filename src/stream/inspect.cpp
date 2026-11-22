#include "inspect.h"

#include <simdjson.h>

#include <string>

namespace llmgw::stream::detail {

namespace od = simdjson::ondemand;

namespace {

// simdjson 要求输入后面有 SIMDJSON_PADDING
// 个可读的字节：复制到线程局部的缓冲区里（事件通常几百字节）
std::string& scratch() {
  thread_local std::string buffer;
  return buffer;
}

simdjson::padded_string_view padded(std::string_view json) {
  std::string& buffer = scratch();
  buffer.assign(json);
  buffer.reserve(json.size() + simdjson::SIMDJSON_PADDING);
  return simdjson::padded_string_view(buffer.data(), json.size(), buffer.capacity());
}

od::parser& parser() {
  thread_local od::parser p;
  return p;
}

// 处理过大的输入（非流式响应体最大 32 MB）之后，释放线程局部的缓冲区和解析器的内存：
// 否则每个 Worker 线程会一直占着最大一次输入的 2–3 倍（独立审查 #7）。声明在函数开头，最后析构
struct ReleaseIfLarge {
  explicit ReleaseIfLarge(std::size_t size) : large(size > (std::size_t{1} << 20)) {}
  ReleaseIfLarge(const ReleaseIfLarge&) = delete;
  ReleaseIfLarge& operator=(const ReleaseIfLarge&) = delete;
  ReleaseIfLarge(ReleaseIfLarge&&) = delete;
  ReleaseIfLarge& operator=(ReleaseIfLarge&&) = delete;
  ~ReleaseIfLarge() {
    if (!large) return;
    scratch() = std::string();
    parser() = od::parser();
  }
  bool large;
};

// 非负整数；null 按 0（与缺少这个字段相同，独立审查 #6）；类型不对或者为负时返回空
std::optional<std::int64_t> count(od::value v) {
  bool is_null = false;
  if (v.is_null().get(is_null) == simdjson::SUCCESS && is_null) return 0;
  std::int64_t n = 0;
  if (v.get_int64().get(n) != simdjson::SUCCESS || n < 0) return std::nullopt;
  return n;
}

// completion_tokens_details.reasoning_tokens；类型不对时返回 false
bool read_reasoning(od::object details, Usage* u) {
  for (auto d : details) {
    std::string_view key;
    if (d.unescaped_key().get(key) != simdjson::SUCCESS) return false;
    if (key != "reasoning_tokens") continue;
    od::value v;
    if (d.value().get(v) != simdjson::SUCCESS) return false;
    auto n = count(v);
    if (!n) return false;
    u->reasoning_tokens = *n;
  }
  return true;
}

// usage 对象 → Usage；任何一个字段类型不对都返回空
std::optional<Usage> read_usage(od::object obj) {
  Usage u;
  for (auto field : obj) {
    std::string_view key;
    if (field.unescaped_key().get(key) != simdjson::SUCCESS) return std::nullopt;
    od::value v;
    if (field.value().get(v) != simdjson::SUCCESS) return std::nullopt;
    if (key == "prompt_tokens" || key == "completion_tokens") {
      auto n = count(v);
      if (!n) return std::nullopt;
      (key == "prompt_tokens" ? u.prompt_tokens : u.completion_tokens) = *n;
    } else if (key == "completion_tokens_details") {
      bool is_null = false;
      if (v.is_null().get(is_null) == simdjson::SUCCESS && is_null)
        continue;  // 没有推理 token 的明细
      od::object details;
      if (v.get_object().get(details) != simdjson::SUCCESS) return std::nullopt;  // 类型不对
      if (!read_reasoning(details, &u)) return std::nullopt;
    }
  }
  u.reported = true;
  return u;
}

}  // namespace

std::optional<UsageFound> extract_usage(std::string_view json) {
  const ReleaseIfLarge release(json.size());
  od::document doc;
  if (parser().iterate(padded(json)).get(doc) != simdjson::SUCCESS) return std::nullopt;
  od::object top;
  if (doc.get_object().get(top) != simdjson::SUCCESS) return std::nullopt;
  std::optional<Usage> usage;
  bool choices_empty = false;
  for (auto field : top) {
    std::string_view key;
    if (field.unescaped_key().get(key) != simdjson::SUCCESS) return std::nullopt;
    od::value v;
    if (field.value().get(v) != simdjson::SUCCESS) return std::nullopt;
    if (key == "choices") {
      od::array arr;
      bool empty = false;
      choices_empty = v.get_array().get(arr) == simdjson::SUCCESS &&
                      arr.is_empty().get(empty) == simdjson::SUCCESS && empty;
    } else if (key == "usage") {
      od::object obj;
      if (v.get_object().get(obj) != simdjson::SUCCESS) {
        usage.reset();  // null（每一块都带 "usage": null）或者类型不对
        continue;
      }
      usage = read_usage(obj);
      if (!usage) return std::nullopt;
    }
  }
  if (!usage) return std::nullopt;
  return UsageFound{.usage = *usage, .usage_only = choices_empty};
}

std::optional<int> mapped_status(std::string_view json, const ErrorMapping& mapping) {
  if (mapping.path.empty() || mapping.codes.empty()) return std::nullopt;
  const ReleaseIfLarge release(json.size());
  // JSON Pointer（RFC 6901）：段中的 "~" 写成 "~0"，"/" 写成 "~1"
  std::string pointer;
  for (const auto& seg : mapping.path) {
    pointer += '/';
    for (const char c : seg) {
      if (c == '~') {
        pointer += "~0";
      } else if (c == '/') {
        pointer += "~1";
      } else {
        pointer += c;
      }
    }
  }
  od::document doc;
  if (parser().iterate(padded(json)).get(doc) != simdjson::SUCCESS) return std::nullopt;
  od::value v;
  if (doc.at_pointer(pointer).get(v) != simdjson::SUCCESS) return std::nullopt;
  std::string text;
  od::json_type type{};
  if (v.type().get(type) != simdjson::SUCCESS) return std::nullopt;
  if (type == od::json_type::string) {
    std::string_view s;
    if (v.get_string().get(s) != simdjson::SUCCESS) return std::nullopt;
    text = std::string(s);
  } else if (type == od::json_type::number) {
    std::string_view raw = v.raw_json_token();
    while (!raw.empty() &&
           (raw.back() == ' ' || raw.back() == '\n' || raw.back() == '\r' || raw.back() == '\t')) {
      raw.remove_suffix(1);
    }
    text = std::string(raw);
  } else {
    return std::nullopt;
  }
  auto it = mapping.codes.find(text);
  if (it == mapping.codes.end()) return std::nullopt;
  return it->second;
}

}  // namespace llmgw::stream::detail
