// 字段路径、URL、监听地址的语法解析。都是纯函数，不依赖 JSON 库。
#include <algorithm>
#include <cctype>
#include <charconv>
#include <optional>
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
    return invalid(
        "不能在 URL 中写用户名和密码（上游的密钥请通过 api_key_env 提供；代理的认证信息写在 proxy "
        "中）");
  }
  auto hp = split_host_port(authority);
  if (!hp) return fail(hp.error());
  if (hp->first.empty()) return invalid("缺少主机名");
  if (hp->first.find('%') != std::string_view::npos) {
    // IPv6 的 zone id（fe80::1%eth0）：证书无法按它校验（任务 1.4 独立审查 R12）
    return invalid("主机名中不能有 %（不支持 IPv6 的 zone id）");
  }
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

namespace {

// 百分号解码（RFC 3986 2.1）。格式错误时返回空
std::optional<std::string> percent_decode(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (std::size_t i = 0; i < text.size(); ++i) {
    if (text[i] != '%') {
      out += text[i];
      continue;
    }
    unsigned value = 0;
    if (i + 2 >= text.size()) return std::nullopt;  // "%" 后面不足两个字符
    const auto* first = text.data() + i + 1;
    auto [p, ec] = std::from_chars(first, first + 2, value, 16);
    if (ec != std::errc{} || p != first + 2) return std::nullopt;
    out += static_cast<char>(value);
    i += 2;
  }
  return out;
}

}  // namespace

Result<ProxyUrl> parse_proxy_url(std::string_view text) {
  // 这里的任何错误信息都不回显原文：用户名和密码可能出现在任何位置
  // （比如漏写了 "@"，密码就被当成了端口号）
  if (text.starts_with("https://")) {
    return invalid("不支持 HTTPS 代理（到代理本身的连接用 TLS）；请使用 http:// 代理");
  }
  if (!text.starts_with("http://")) return invalid("只支持 HTTP 代理，必须以 http:// 开头");
  const std::string_view rest = text.substr(7);
  const auto slash = rest.find('/');
  std::string_view authority = rest.substr(0, slash);
  if (slash != std::string_view::npos &&
      rest.substr(slash).find_first_not_of('/') != std::string_view::npos) {
    return invalid("代理地址不能带路径，应为 http://[用户名:密码@]主机[:端口]");
  }
  ProxyUrl proxy;
  if (const auto at = authority.rfind('@'); at != std::string_view::npos) {
    const std::string_view userinfo = authority.substr(0, at);
    authority = authority.substr(at + 1);
    const auto colon = userinfo.find(':');
    auto user = percent_decode(userinfo.substr(0, colon));
    auto pass = percent_decode(colon == std::string_view::npos ? std::string_view{}
                                                               : userinfo.substr(colon + 1));
    if (!user || !pass) return invalid("代理的用户名或密码中有不合法的百分号编码（%XX）");
    if (user->empty()) return invalid("代理地址中 \"@\" 前面缺少用户名");
    proxy.username = std::move(*user);
    proxy.password = std::move(*pass);
  }
  auto url = parse_url("http://" + std::string(authority));
  if (!url) return invalid("代理地址格式不对，应为 http://[用户名:密码@]主机[:端口]");
  proxy.url = std::move(*url);
  return proxy;
}

bool no_proxy_matches(std::string_view entry, std::string_view host) {
  if (entry == "*") return true;
  // 完全限定的写法（api.example.com.）：两边都去掉结尾的点（只去一边时，带点的项连自己都匹配不上，模糊测试发现）
  while (host.ends_with('.')) host.remove_suffix(1);
  while (entry.ends_with('.')) entry.remove_suffix(1);
  if (entry.empty() || host.size() < entry.size()) return false;
  auto lower_equal = [](std::string_view a, std::string_view b) {
    return std::equal(a.begin(), a.end(), b.begin(), b.end(), [](char x, char y) {
      return std::tolower(static_cast<unsigned char>(x)) ==
             std::tolower(static_cast<unsigned char>(y));
    });
  };
  if (host.size() == entry.size()) return lower_equal(host, entry);
  // 子域名：entry 为 example.com 时匹配 api.example.com，但不匹配 badexample.com
  return host[host.size() - entry.size() - 1] == '.' &&
         lower_equal(host.substr(host.size() - entry.size()), entry);
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
