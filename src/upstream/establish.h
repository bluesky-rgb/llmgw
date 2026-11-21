// 建立一条新连接：DNS → TCP →（代理 CONNECT）→（TLS 握手）。
#pragma once

#include <openssl/ssl.h>

#include <boost/asio/ssl/context.hpp>
#include <memory>
#include <string>

#include "connection.h"
#include "dns.h"
#include "llmgw/config/snapshot.h"
#include "llmgw/upstream/client.h"

namespace llmgw::upstream::detail {

/// 一个上游的连接方式。每次 send() 时从配置算出（只是几个指针）。
struct Route {
  const config::Provider* provider = nullptr;
  const config::ProxyUrl* proxy = nullptr;   ///< 为空表示直连
  boost::asio::ssl::context* tls = nullptr;  ///< 为空表示 http
  /// HTTP 上游经过代理：不建隧道，请求行用绝对路径形式，每个请求带 Proxy-Authorization
  [[nodiscard]] bool absolute_form() const { return proxy != nullptr && tls == nullptr; }
};

/// 设置 TLS 要校验的证书身份（主机名，或 IP 地址）和 SNI。任何一步失败都返回 false，
/// 调用者必须中止握手：忽略返回值会让身份校验被静默关闭，任何受信任 CA 签发的证书都能通过（独立审查
/// R12）
bool set_tls_identity(SSL* ssl, const std::string& host);

/// host:port，IPv6 地址加方括号
std::string authority(const std::string& host, std::uint16_t port);
/// Basic 认证的请求头的值
std::string basic_auth(const std::string& user, const std::string& password);

/// 建立连接。progress 随进度更新为当前阶段：调用者的超时定时器先到时，用它判断超时发生在哪个阶段。
/// 失败的 maybe_sent 都是 false（请求还没有开始发送）。
Task<Outcome<std::unique_ptr<Connection>>> establish(Route route, DnsCache* dns, Stage* progress);

}  // namespace llmgw::upstream::detail
