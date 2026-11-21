// 配置的加载与校验。
//
// 重点是错误提示的质量：一次列出所有问题，每个问题指出 JSON 路径，
// 拼错的名字给出“是不是 xxx”的建议。设计见 docs/tasks/1.1-config.md。
//
// 加载只在启动（以及以后的热加载）时执行，不在热路径上。
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "llmgw/config/snapshot.h"
#include "llmgw/core/result.h"

namespace llmgw::config {

enum class Severity : std::uint8_t { kError, kWarning };

/// 配置中的一个问题。
struct Issue {
  Severity severity = Severity::kError;
  /// 出问题的位置：JSON 路径（"models[0].targets[1].provider"），
  /// 语法错误时为“第 N 行第 M 列”，文件读取失败时为文件名。
  std::string path;
  /// 说明，以及建议怎么改。不包含密钥。
  std::string message;
};

/// 读取环境变量的函数：变量不存在时返回 std::nullopt。
using EnvLookup = std::function<std::optional<std::string>(const std::string&)>;

struct LoadOptions {
  /// false：不读取环境变量，也不检查密钥是否存在（llmgw check --skip-env，用于 CI）。
  bool resolve_env = true;
  /// 读取环境变量；为空时读取真实的进程环境。单元测试中注入假的实现。
  EnvLookup getenv;
};

/// 加载结果。
struct LoadReport {
  /// 配置有效时为快照；有任何错误时为空。
  std::shared_ptr<const Snapshot> snapshot;
  /// 所有错误和警告。按部分的处理顺序排列：server、timeouts、providers、proxy、models，
  /// 每部分内按字段在文件中的顺序；跨部分的检查（引用关系、重名等）排在最后。
  std::vector<Issue> issues;

  [[nodiscard]] bool ok() const { return snapshot != nullptr; }
  [[nodiscard]] std::size_t error_count() const;
  [[nodiscard]] std::size_t warning_count() const;
};

/// 从 JSON 文本加载。
LoadReport parse(std::string_view json_text, const LoadOptions& options = {});

/// 从文件加载。文件读取失败时返回一个错误，路径为文件名。
LoadReport load_file(const std::string& path, const LoadOptions& options = {});

/// 解析字段路径，比如 "messages[].reasoning_content"。
/// 语法：段 ("." 段)*，段 = 名字 ["[]"]，名字由字母、数字、"_"、"-" 组成。
Result<FieldPath> parse_field_path(std::string_view text);

/// 字段路径转回文本（用于错误信息和日志）。
std::string to_string(const FieldPath& path);

/// 解析 http(s) URL。不允许查询参数、片段和用户信息。
Result<Url> parse_url(std::string_view text);

/// 解析监听地址 "host:port"（IPv6 写作 "[::1]:9090"）。
Result<Endpoint> parse_endpoint(std::string_view text);

}  // namespace llmgw::config
