// 字段路径、URL、监听地址的语法解析。都是纯函数，不依赖 JSON 库。
#include <cctype>
#include <charconv>
#include <string>
#include <string_view>

#include "llmgw/config/loader.h"

namespace llmgw::config {
namespace {

bool is_name_char(char c) {
  return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_' || c == '-';
}

Failure invalid(std::string message) {
  return fail(ErrorCode::kInvalidRequest, std::move(message));
}

// 解析 1–65535 的端口号
Result<std::uint16_t> parse_port(std::string_view text) {
  unsigned value = 0;
  const auto* end = text.data() + text.size();
  auto [p, ec] = std::from_chars(text.data(), end, value);
  if (text.empty() || ec != std::errc{} || p != end || value == 0 || value > 65535) {
    return invalid("端口必须是 1–65535 之间的整数，实际是 \"" + std::string(text) + "\"");
  }
  return static_cast<std::uint16_t>(value);
}

// 把 "host:port" 或 "[v6]:port" 拆开；port 部分可以没有（返回空）
Result<std::pair<std::string_view, std::string_view>> split_host_port(std::string_view text) {
  if (text.starts_with('[')) {
    const auto close = text.find(']');
    if (close == std::string_view::npos) return invalid("IPv6 地址缺少 \"]\"");
    const auto host = text.substr(1, close - 1);
    const auto rest = text.substr(close + 1);
    if (rest.empty()) return std::pair{host, std::string_view{}};
    if (!rest.starts_with(':')) return invalid("\"]\" 后面只能是 \":端口\"");
    return std::pair{host, rest.substr(1)};
  }
  const auto colon = text.rfind(':');
  if (colon == std::string_view::npos) return std::pair{text, std::string_view{}};
  if (text.find(':') != colon) return invalid("IPv6 地址需要写在方括号中，比如 [::1]:9090");
  return std::pair{text.substr(0, colon), text.substr(colon + 1)};
}

}  // namespace

Result<FieldPath> parse_field_path(std::string_view text) {
  if (text.empty()) return invalid("字段路径不能为空");
  FieldPath path;
  std::size_t pos = 0;
  while (true) {
    std::size_t end = pos;
    while (end < text.size() && is_name_char(text[end])) ++end;
    if (end == pos) {
      return invalid("字段路径 \"" + std::string(text) + "\" 的第 " + std::to_string(pos + 1) +
                     " 个字符处缺少字段名（字段名由字母、数字、_ 和 - 组成）");
    }
    PathSegment seg{.name = std::string(text.substr(pos, end - pos)), .each = false};
    if (text.substr(end).starts_with("[]")) {
      seg.each = true;
      end += 2;
    }
    path.push_back(std::move(seg));
    if (end == text.size()) return path;
    if (text[end] != '.') {
      return invalid("字段路径 \"" + std::string(text) + "\" 的第 " + std::to_string(end + 1) +
                     " 个字符应该是 \".\"；数组写作 \"messages[]\"，不支持下标");
    }
    pos = end + 1;
  }
}

std::string to_string(const FieldPath& path) {
  std::string out;
  for (const auto& seg : path) {
    if (!out.empty()) out += '.';
    out += seg.name;
    if (seg.each) out += "[]";
  }
  return out;
}

Result<Url> parse_url(std::string_view text) {
  Url url;
  std::string_view rest;
  if (text.starts_with("https://")) {
    url.scheme = "https";
    url.port = 443;
    rest = text.substr(8);
  } else if (text.starts_with("http://")) {
    url.scheme = "http";
    url.port = 80;
    rest = text.substr(7);
  } else {
    // 含有 "@" 时可能带有凭据（user:pass@host），不回显原文（独立审查发现）
    if (text.find('@') != std::string_view::npos) return invalid("必须以 http:// 或 https:// 开头");
    return invalid("必须以 http:// 或 https:// 开头，实际是 \"" + std::string(text) + "\"");
  }
  if (rest.find_first_of("?#") != std::string_view::npos)
    return invalid("不能包含查询参数（?）或片段（#）");
  const auto slash = rest.find('/');
  const auto authority = rest.substr(0, slash);
  if (authority.find('@') != std::string_view::npos) {
    return invalid("不能在 URL 中写用户名和密码（上游的密钥请通过 api_key_env 提供；代理认证将在任务 1.4 中支持）");
  }
  auto hp = split_host_port(authority);
  if (!hp) return fail(hp.error());
  if (hp->first.empty()) return invalid("缺少主机名");
  url.host = std::string(hp->first);
  if (!hp->second.empty()) {
    auto port = parse_port(hp->second);
    if (!port) return fail(port.error());
    url.port = *port;
  }
  if (slash != std::string_view::npos) {
    auto path = rest.substr(slash);
    while (path.ends_with('/')) path.remove_suffix(1);
    url.base_path = std::string(path);
  }
  return url;
}

Result<Endpoint> parse_endpoint(std::string_view text) {
  auto hp = split_host_port(text);
  if (!hp) return fail(hp.error());
  if (hp->first.empty()) return invalid("缺少主机名，比如 \"127.0.0.1:9090\"");
  if (hp->second.empty()) return invalid("缺少端口，比如 \"127.0.0.1:9090\"");
  auto port = parse_port(hp->second);
  if (!port) return fail(port.error());
  return Endpoint{.host = std::string(hp->first), .port = *port};
}

}  // namespace llmgw::config
