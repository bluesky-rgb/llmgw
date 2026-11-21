#include "json_reader.h"

#include <algorithm>
#include <limits>
#include <numeric>
#include <utility>

namespace llmgw::config::detail {

void Diagnostics::error(std::string path, std::string message) {
  issues_.push_back(
      {.severity = Severity::kError, .path = std::move(path), .message = std::move(message)});
}

void Diagnostics::warning(std::string path, std::string message) {
  issues_.push_back(
      {.severity = Severity::kWarning, .path = std::move(path), .message = std::move(message)});
}

bool Diagnostics::has_errors() const {
  return std::any_of(issues_.begin(), issues_.end(),
                     [](const Issue& i) { return i.severity == Severity::kError; });
}

std::size_t edit_distance(std::string_view a, std::string_view b) {
  std::vector<std::size_t> prev(b.size() + 1);
  std::vector<std::size_t> cur(b.size() + 1);
  std::iota(prev.begin(), prev.end(), 0);
  for (std::size_t i = 1; i <= a.size(); ++i) {
    cur[0] = i;
    for (std::size_t j = 1; j <= b.size(); ++j) {
      const std::size_t sub = prev[j - 1] + (a[i - 1] == b[j - 1] ? 0 : 1);
      cur[j] = std::min({prev[j] + 1, cur[j - 1] + 1, sub});
    }
    std::swap(prev, cur);
  }
  return prev[b.size()];
}

std::string closest(std::string_view word, const std::vector<std::string>& candidates) {
  std::string best;
  std::size_t best_dist = 3;  // 只接受距离 ≤ 2 的建议
  for (const auto& c : candidates) {
    const auto d = edit_distance(word, c);
    if (d < best_dist) {
      best_dist = d;
      best = c;
    }
  }
  return best;
}

std::string did_you_mean(std::string_view word, const std::vector<std::string>& candidates) {
  const auto best = closest(word, candidates);
  return best.empty() ? std::string{} : "（是不是 \"" + best + "\"？）";
}

std::string type_name(const Json& value) {
  switch (value.type()) {
    case Json::value_t::null:
      return "null";
    case Json::value_t::object:
      return "对象";
    case Json::value_t::array:
      return "数组";
    case Json::value_t::string:
      return "字符串";
    case Json::value_t::boolean:
      return "布尔值";
    case Json::value_t::number_integer:
    case Json::value_t::number_unsigned:
      return "整数";
    case Json::value_t::number_float:
      return "小数";
    case Json::value_t::binary:
    case Json::value_t::discarded:
      return "其他";
  }
  return "其他";
}

std::string index_path(const std::string& array_path, std::size_t index) {
  return array_path + "[" + std::to_string(index) + "]";
}

ObjectReader::ObjectReader(const Json& value, std::string path, Diagnostics* diag)
    : path_(std::move(path)), diag_(diag) {
  if (value.is_object()) {
    obj_ = &value;
  } else {
    diag_->error(path_.empty() ? "（顶层）" : path_,
                 "应该是一个对象 {…}，实际是" + type_name(value));
  }
}

std::string ObjectReader::path_of(std::string_view key) const {
  return path_.empty() ? std::string(key) : path_ + "." + std::string(key);
}

namespace {
void add_unique(std::vector<std::string>* list, std::string_view key) {
  if (std::find(list->begin(), list->end(), key) == list->end()) list->emplace_back(key);
}
}  // namespace

const Json* ObjectReader::lookup(std::string_view key) {
  add_unique(&handled_, key);
  add_unique(&suggestible_, key);
  return find(key);
}

const Json* ObjectReader::find(std::string_view key) const {
  if (obj_ == nullptr) return nullptr;
  auto it = obj_->find(key);
  return it == obj_->end() ? nullptr : &*it;
}

bool ObjectReader::has(std::string_view key) { return lookup(key) != nullptr; }

std::optional<std::string> ObjectReader::string(std::string_view key, bool required) {
  const Json* v = lookup(key);
  if (v == nullptr) {
    if (required && valid()) missing(key);
    return std::nullopt;
  }
  if (!v->is_string()) {
    diag_->error(path_of(key), "应该是字符串，实际是" + type_name(*v));
    return std::nullopt;
  }
  return v->get<std::string>();
}

std::optional<std::int64_t> ObjectReader::integer(std::string_view key, std::int64_t min,
                                                  std::int64_t max, bool required) {
  const Json* v = lookup(key);
  if (v == nullptr) {
    if (required && valid()) missing(key);
    return std::nullopt;
  }
  const std::string range = "，允许的范围是 " + std::to_string(min) + "–" + std::to_string(max);
  if (v->is_number_float()) {
    diag_->error(path_of(key), "应该是整数，实际是小数 " + v->dump() + range);
    return std::nullopt;
  }
  if (!v->is_number_integer()) {
    diag_->error(path_of(key), "应该是整数，实际是" + type_name(*v) + range);
    return std::nullopt;
  }
  if (v->is_number_unsigned() &&
      v->get<std::uint64_t>() >
          static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
    diag_->error(path_of(key), "数值太大" + range);
    return std::nullopt;
  }
  const auto n = v->get<std::int64_t>();
  if (n < min || n > max) {
    diag_->error(path_of(key), "取值 " + std::to_string(n) + " 超出范围" + range);
    return std::nullopt;
  }
  return n;
}

std::optional<bool> ObjectReader::boolean(std::string_view key) {
  const Json* v = lookup(key);
  if (v == nullptr) return std::nullopt;
  if (!v->is_boolean()) {
    diag_->error(path_of(key), "应该是 true 或 false，实际是" + type_name(*v));
    return std::nullopt;
  }
  return v->get<bool>();
}

std::optional<ObjectReader> ObjectReader::object(std::string_view key, bool required) {
  const Json* v = lookup(key);
  if (v == nullptr) {
    if (required && valid()) missing(key);
    return std::nullopt;
  }
  ObjectReader child(*v, path_of(key), diag_);
  if (!child.valid()) return std::nullopt;
  return child;
}

const Json* ObjectReader::array(std::string_view key, bool required) {
  const Json* v = lookup(key);
  if (v == nullptr) {
    if (required && valid()) missing(key);
    return nullptr;
  }
  if (!v->is_array()) {
    diag_->error(path_of(key), "应该是数组 […]，实际是" + type_name(*v));
    return nullptr;
  }
  return v;
}

const Json* ObjectReader::raw(std::string_view key) { return lookup(key); }

void ObjectReader::missing(std::string_view key) { add_unique(&missing_, key); }

void ObjectReader::unsupported(std::string_view key, std::string_view when, std::string_view note) {
  // 只登记为“已处理”，不作为拼写建议：不应该建议用户去写一个尚未支持的字段
  add_unique(&handled_, key);
  if (find(key) != nullptr) {
    std::string msg = "\"" + std::string(key) + "\" 尚未支持（" + std::string(when) +
                      "）。请先删除这一项，避免误以为它已经生效";
    if (!note.empty()) msg += "。" + std::string(note);
    diag_->error(path_of(key), std::move(msg));
  }
}

void ObjectReader::finish() {
  if (obj_ == nullptr) return;
  std::string allowed;
  for (const auto& s : suggestible_) allowed += (allowed.empty() ? "" : ", ") + s;
  std::vector<std::string> unexplained = missing_;  // 没有被“拼写错误”解释掉的缺失字段
  for (const auto& [key, value] : obj_->items()) {
    if (std::find(handled_.begin(), handled_.end(), key) != handled_.end()) continue;
    const auto best = closest(key, suggestible_);
    auto it = std::find(unexplained.begin(), unexplained.end(), best);
    if (!best.empty() && it != unexplained.end()) {
      diag_->error(path_of(key), "未知字段，应该是必填字段 \"" + best + "\"？");
      unexplained.erase(it);
      continue;
    }
    diag_->error(path_of(key),
                 "未知字段" + did_you_mean(key, suggestible_) + "。允许的字段：" + allowed);
  }
  for (const auto& key : unexplained) diag_->error(path_of(key), "缺少必填字段");
}

}  // namespace llmgw::config::detail
