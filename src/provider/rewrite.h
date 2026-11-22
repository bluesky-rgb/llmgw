// 请求体的改写器（任务 1.6 第 3、4 节）：基于字节位置，只重建改写路径经过的对象，
// 其余的值逐字节保留。1 MB 的请求体约 0.7 ms（DOM 方案约 10 ms）。
#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "llmgw/config/snapshot.h"
#include "llmgw/core/result.h"

namespace llmgw::provider::detail {

/// 一个目标的全部改写。
struct Rules {
  std::string model;           ///< 内置：顶层 model 换成这个
  bool include_usage = false;  ///< 内置：设置 stream_options.include_usage = true（流式请求）
  std::vector<config::FieldPath> drops;
  std::vector<config::FieldRename> renames;
  std::vector<config::FieldSet> sets;
};

struct Rewritten {
  std::string body;
  /// 网关加上了 include_usage（客户端自己没有要求）：stream 应该去掉 usage 块
  bool injected_include_usage = false;
};

/// 改写请求体。请求体必须是 JSON 对象；改写路径经过的部分不是合法的 JSON 时返回错误（400）。
///
/// 顺序与语义（逐层进行）：内置改写 → drop_fields → rename_fields → set_fields。
/// - 键按解码后的名字匹配；重复的键：每一个同名项都改写
/// - 改名的目标名已经存在时，原来的那一项被删除
/// - 设置：替换每一个同名的项；一项都没有时创建（中间的对象一起创建）；
///   路径经过的值不是对象时替换为对象
/// - 带 [] 的段：对数组中的每个对象元素生效；不是数组、元素不是对象时跳过
/// - 内置改写与配置的任何一条规则在同一条线上（一条是另一条的前缀）时，让给配置
/// - 规则的路径不会经过被改名或者被设置的字段（配置加载时拒绝），所以不存在“按原名还是新名”的问题
/// - injected_include_usage：发给上游的请求要求了 usage、客户端的请求没有要求（任意一项为 true
/// 即为要求了）
Result<Rewritten> rewrite(std::string_view body, const Rules& rules);

/// JSON 字符串字面量（带引号）
std::string json_string(std::string_view s);

}  // namespace llmgw::provider::detail
