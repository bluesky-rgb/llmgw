// 内部：带 JSON 路径的类型化读取、未知字段检测、“是不是 xxx”的建议。
//
// 用法：
//   ObjectReader r(json, "server", &diag);
//   auto port = r.integer("port", 1, 65535);   // 不存在返回 nullopt；类型或范围错误时记录问题
//   ...
//   r.finish();                                // 报告没有被读过的字段（即未知字段）
//
// 每个读取函数都把字段名登记为“已知”，finish() 时对象里剩下的字段就是未知字段，
// 并从已知字段中找出最接近的名字作为建议。这样各部分的解析代码不需要各自处理这两件事。
#pragma once

#include <cstdint>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "llmgw/config/loader.h"

namespace llmgw::config::detail {

// 保留字段在文件中的顺序，未知字段按出现顺序报告
using Json = nlohmann::ordered_json;

/// 收集问题。
class Diagnostics {
 public:
  void error(std::string path, std::string message);
  void warning(std::string path, std::string message);
  [[nodiscard]] bool has_errors() const;
  [[nodiscard]] std::vector<Issue>& issues() { return issues_; }

 private:
  std::vector<Issue> issues_;
};

/// 两个字符串的编辑距离（用于拼写建议）。
std::size_t edit_distance(std::string_view a, std::string_view b);

/// 在候选中找与 word 最接近、编辑距离不超过 2 的一个；没有则返回空。
std::string closest(std::string_view word, const std::vector<std::string>& candidates);

/// “（是不是 "xxx"？）”；没有建议时返回空字符串。
std::string did_you_mean(std::string_view word, const std::vector<std::string>& candidates);

/// JSON 类型的中文名（用于错误信息）。
std::string type_name(const Json& value);

/// 读取一个 JSON 对象。
class ObjectReader {
 public:
  /// value 不是对象时记录一个错误，之后的读取都返回 nullopt。
  ObjectReader(const Json& value, std::string path, Diagnostics* diag);

  [[nodiscard]] bool valid() const { return obj_ != nullptr; }
  [[nodiscard]] const std::string& path() const { return path_; }
  [[nodiscard]] std::string path_of(std::string_view key) const;
  [[nodiscard]] Diagnostics* diag() const { return diag_; }

  /// 字段是否存在（同时登记为已知字段）。
  bool has(std::string_view key);

  std::optional<std::string> string(std::string_view key, bool required = false);
  /// 整数，范围 [min, max]；小数、负数、超出范围都报错。
  std::optional<std::int64_t> integer(std::string_view key, std::int64_t min, std::int64_t max,
                                      bool required = false);
  std::optional<bool> boolean(std::string_view key);
  /// 子对象。不存在时返回 nullopt；存在但不是对象时报错并返回 nullopt。
  std::optional<ObjectReader> object(std::string_view key, bool required = false);
  /// 数组。不存在时返回 nullptr；存在但不是数组时报错并返回 nullptr。
  const Json* array(std::string_view key, bool required = false);
  /// 任意类型的原始值（不检查类型）。不存在时返回 nullptr。
  const Json* raw(std::string_view key);

  /// 已知但尚未支持的字段：出现时报错，说明在哪个版本支持；note 是额外的提醒（可以为空）。
  void unsupported(std::string_view key, std::string_view when, std::string_view note = {});

  /// 报告缺少的必填字段和没有被读过的字段（未知字段）。每个对象读完后调用一次。
  /// 未知字段恰好是某个缺少的必填字段的拼写错误时（base_ulr / base_url），合并成一条。
  void finish();

 private:
  // 查找并登记为已知字段
  const Json* lookup(std::string_view key);
  // 只查找，不登记
  [[nodiscard]] const Json* find(std::string_view key) const;
  // 登记一个缺少的必填字段，在 finish() 中报告
  void missing(std::string_view key);

  const Json* obj_ = nullptr;
  std::string path_;
  Diagnostics* diag_;
  std::vector<std::string> handled_;  // 处理过的字段：finish() 时不算未知字段
  std::vector<std::string> suggestible_;  // 可以作为拼写建议的字段（不含“尚未支持”的字段）
  std::vector<std::string> missing_;  // 缺少的必填字段
};

/// 数组元素的路径，比如 "models[2]"。
std::string index_path(const std::string& array_path, std::size_t index);

}  // namespace llmgw::config::detail
