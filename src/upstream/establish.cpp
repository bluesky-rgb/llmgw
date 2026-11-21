#include "establish.h"

#include <openssl/ssl.h>
#include <openssl/x509v3.h>

#include <array>
#include <boost/asio/as_tuple.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/beast/http.hpp>

namespace llmgw::upstream::detail {

namespace http = boost::beast::http;
namespace ssl = boost::asio::ssl;
constexpr auto kTuple = asio::as_tuple(asio::use_awaitable);

namespace {

auto failure(Stage stage, std::string detail) {
  return unexpected(AttemptError{.stage = stage, .detail = std::move(detail)});
}

bool is_ip(const std::string& host) {
  boost::system::error_code ec;
  (void)asio::ip::make_address(host, ec);
  return !ec;
}

// 代理握手：CONNECT host:port，期望 2xx。之后这条 TCP 连接就是到上游的隧道
Task<Outcome<void>> proxy_connect(Connection* conn, const config::Url* target,
                                  const config::ProxyUrl* proxy) {
  const std::string dest = authority(target->host, target->port);
  http::request<http::empty_body> req{http::verb::connect, dest, 11};
  req.set(http::field::host, dest);
  if (!proxy->username.empty()) {
    req.set(http::field::proxy_authorization, basic_auth(proxy->username, proxy->password));
  }
  auto [wec, wn] = co_await http::async_write(conn->socket(), req, kTuple);
  if (wec) co_return failure(Stage::kProxy, "向代理发送 CONNECT 失败：" + wec.message());
  http::response_parser<http::empty_body> parser;
  parser.skip(true);  // CONNECT 的 2xx 响应没有响应体，之后的字节属于隧道
  parser.header_limit(16 * 1024);
  auto [rec, rn] = co_await http::async_read_header(conn->socket(), conn->buffer(), parser, kTuple);
  if (rec) co_return failure(Stage::kProxy, "读取代理的 CONNECT 响应失败：" + rec.message());
  const unsigned status = parser.get().result_int();
  if (status / 100 != 2) {
    std::string why = "代理拒绝了 CONNECT，状态码 " + std::to_string(status);
    if (status == 407) why += "（代理要求认证，或者用户名密码不对）";
    co_return failure(Stage::kProxy, why);
  }
  if (conn->buffer().size() > 0) co_return failure(Stage::kProxy, "代理在隧道建立之前多发了数据");
  co_return Outcome<void>{};
}

Task<Outcome<void>> tls_handshake(Connection* conn, ssl::context* ctx, std::string host) {
  conn->start_tls(*ctx);
  SSL* s = conn->tls().native_handle();
  if (!set_tls_identity(s, host)) {
    co_return failure(Stage::kTls, "无法设置要校验的证书身份（主机名或 IP 地址不合法）");
  }
  auto [ec] = co_await conn->tls().async_handshake(ssl::stream_base::client, kTuple);
  if (!ec) co_return Outcome<void>{};
  const long verify = SSL_get_verify_result(s);
  if (verify != X509_V_OK) {
    co_return failure(Stage::kTls,
                      std::string("证书校验失败：") + X509_verify_cert_error_string(verify));
  }
  co_return failure(Stage::kTls, "TLS 握手失败：" + ec.message());
}

}  // namespace

bool set_tls_identity(SSL* ssl, const std::string& host) {
  if (host.find('%') != std::string::npos) return false;  // IPv6 的 zone id：OpenSSL 无法按它校验
  if (is_ip(host)) {
    // IP 地址：不发送 SNI（RFC 6066），证书按 IP 校验
    return X509_VERIFY_PARAM_set1_ip_asc(SSL_get0_param(ssl), host.c_str()) == 1;
  }
  return SSL_set_tlsext_host_name(ssl, host.c_str()) == 1 && SSL_set1_host(ssl, host.c_str()) == 1;
}

std::string authority(const std::string& host, std::uint16_t port) {
  const bool v6 = host.find(':') != std::string::npos;
  return (v6 ? "[" + host + "]" : host) + ":" + std::to_string(port);
}

std::string basic_auth(const std::string& user, const std::string& password) {
  static constexpr std::string_view kAlphabet =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  const std::string in = user + ":" + password;
  std::string out = "Basic ";
  std::size_t i = 0;
  for (; i + 2 < in.size(); i += 3) {
    const auto n = (static_cast<unsigned char>(in[i]) << 16U) |
                   (static_cast<unsigned char>(in[i + 1]) << 8U) |
                   static_cast<unsigned char>(in[i + 2]);
    out += kAlphabet[(n >> 18U) & 63U];
    out += kAlphabet[(n >> 12U) & 63U];
    out += kAlphabet[(n >> 6U) & 63U];
    out += kAlphabet[n & 63U];
  }
  if (const std::size_t rest = in.size() - i; rest > 0) {
    unsigned n = static_cast<unsigned char>(in[i]) << 16U;
    if (rest == 2) n |= static_cast<unsigned char>(in[i + 1]) << 8U;
    out += kAlphabet[(n >> 18U) & 63U];
    out += kAlphabet[(n >> 12U) & 63U];
    out += rest == 2 ? kAlphabet[(n >> 6U) & 63U] : '=';
    out += '=';
  }
  return out;
}

Task<Outcome<std::unique_ptr<Connection>>> establish(Route route, DnsCache* dns, Stage* progress) {
  const config::Url& target = route.provider->base_url;
  const config::Url& first_hop = route.proxy != nullptr ? route.proxy->url : target;

  *progress = Stage::kResolve;
  auto endpoints = co_await dns->resolve(first_hop.host, first_hop.port);
  if (!endpoints)
    co_return failure(Stage::kResolve, endpoints.error().message + "（" + first_hop.host + "）");

  *progress = Stage::kConnect;
  tcp::socket socket(co_await asio::this_coro::executor);
  auto [cec, ep] = co_await asio::async_connect(socket, *endpoints, kTuple);
  if (cec) {
    co_return failure(Stage::kConnect, "连接 " + authority(first_hop.host, first_hop.port) +
                                           (route.proxy != nullptr ? "（代理）" : "") + " 失败：" +
                                           cec.message());
  }
  boost::system::error_code ignored;
  socket.set_option(tcp::no_delay(true), ignored);
  auto conn = std::make_unique<Connection>(std::move(socket));

  if (route.proxy != nullptr && route.tls != nullptr) {
    *progress = Stage::kProxy;
    if (auto r = co_await proxy_connect(conn.get(), &target, route.proxy); !r)
      co_return unexpected(r.error());
  }
  if (route.tls != nullptr) {
    *progress = Stage::kTls;
    if (auto r = co_await tls_handshake(conn.get(), route.tls, target.host); !r)
      co_return unexpected(r.error());
  }
  co_return conn;
}

}  // namespace llmgw::upstream::detail
