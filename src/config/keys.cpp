// 配置的 keys 部分（任务 2.2）：网关 key 的标识和哈希。
//
// 规则：配置中只能出现 key 的哈希。任何时候都不在错误信息中回显 hash 字段的值——它可能就是 key 本身
// （写错了位置），错误信息会出现在终端、CI 日志里。
#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>

#include "sections.h"

namespace llmgw::config::detail {
namespace {

constexpr std::string_view kPrefix = "sha256:";
constexpr std::size_t kHexLength = 64;
constexpr std::size_t kMaxIdLength = 64;

bool valid_id(std::string_view id) {
  return !id.empty() && id.size() <= kMaxIdLength && std::ranges::all_of(id, [](char c) {
    return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '-' || c == '_' || c == '.';
  });
}

// 规范化成小写；格式不对时返回空
std::string normalize_hash(std::string_view hash) {
  if (!hash.starts_with(kPrefix) || hash.size() != kPrefix.size() + kHexLength) return {};
  std::string out(kPrefix);
  for (const char c : hash.substr(kPrefix.size())) {
    if (std::isxdigit(static_cast<unsigned char>(c)) == 0) return {};
    out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  }
  return out;
}

}  // namespace

KeyConfig read_key(ObjectReader r) {
  KeyConfig k;
  if (auto id = r.string("id", true)) {
    if (id->starts_with("sk-")) {
      // id 会出现在日志、指标和用量中：把 key 写在这里同样是泄露（任务 2.2 独立审查 #2）
      r.diag()->error(
          r.path_of("id"),
          "这像是 key 本身，不能作为 id（id 会出现在日志和指标中）；id 不能以 \"sk-\" 开头。"
          "如果这是一个 key，请当作已泄露，重新生成一个");
    } else if (valid_id(*id)) {
      k.id = std::move(*id);
    } else {
      r.diag()->error(r.path_of("id"),
                      "只能包含字母、数字和 \"-_.\"，长度 1–64（日志和指标中用它标识 key）");
    }
  }
  if (r.has("key")) {
    r.diag()->error(
        r.path_of("key"),
        "配置中不能写 key 本身，只能写它的哈希（hash 字段）。用 llmgw keygen 生成 key 和哈希；"
        "这个 key 已经出现在配置文件中，请当作已泄露，重新生成一个");
  }
  if (auto hash = r.string("hash", true)) {
    if (hash->starts_with("sk-")) {
      r.diag()->error(r.path_of("hash"),
                      "这是 key 本身，不是哈希：配置中只能写哈希。用 llmgw keygen 生成 key 和哈希；"
                      "这个 key 已经出现在配置文件中，请当作已泄露，重新生成一个");
    } else if (auto normalized = normalize_hash(*hash); normalized.empty()) {
      r.diag()->error(r.path_of("hash"),
                      "格式应该是 \"sha256:\" 加 64 位十六进制（llmgw keygen 输出的就是这个格式）");
    } else {
      k.hash = std::move(normalized);
    }
  }
  r.unsupported("limits", "将在 v0.3 提供，任务 2.3");
  r.unsupported("budget", "将在 v0.3 提供，任务 2.4");
  r.finish();
  return k;
}

void check_keys(const std::vector<KeyConfig>& keys, Diagnostics* diag) {
  for (std::size_t i = 0; i < keys.size(); ++i) {
    for (std::size_t j = 0; j < i; ++j) {
      if (!keys[i].id.empty() && keys[i].id == keys[j].id) {
        diag->error(index_path("keys", i) + ".id", "与 " + index_path("keys", j) + " 的 id 重名");
      }
      if (!keys[i].hash.empty() && keys[i].hash == keys[j].hash) {
        diag->error(index_path("keys", i) + ".hash",
                    "与 " + index_path("keys", j) + " 的哈希相同：同一个 key 不能对应两个 id");
      }
    }
  }
}

}  // namespace llmgw::config::detail
