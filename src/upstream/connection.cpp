#include "connection.h"

#include <openssl/ssl.h>
#include <sys/socket.h>

#include <cerrno>

namespace llmgw::upstream::detail {

bool Connection::healthy() {
  tcp::socket& s = socket();
  if (!s.is_open()) return false;
  // 上一个响应之后还有多余的字节：上游发来了我们没有请求的数据，连接的状态已经不可信
  if (buffer_.size() > 0) return false;
  if (tls_) {
    // TLS 层可能还有上游多发的记录：已经从套接字读进来、在 OpenSSL 的缓冲区或 Asio 的 BIO 对里，
    // 套接字上已经看不到了（独立审查 R1：下一个请求收到了不属于它的响应）。
    // SSL_has_pending 包括还没有解密的记录；BIO_ctrl_pending 是 BIO 对里还没交给 OpenSSL 的密文
    SSL* ssl = tls_->native_handle();
    if (SSL_pending(ssl) > 0 || SSL_has_pending(ssl) != 0) return false;
    if (BIO* rbio = SSL_get_rbio(ssl); rbio != nullptr && BIO_ctrl_pending(rbio) > 0) return false;
  }
  char byte = 0;
  const auto n = ::recv(s.native_handle(), &byte, 1, MSG_PEEK | MSG_DONTWAIT);
  if (n == 0) return false;  // 上游已经关闭（FIN）
  // 有数据：上游在空闲时发来了东西（多半是 TLS 的 close_notify 或者 TLS 1.3 的会话票据）。
  // 保守处理：不复用。误判的后果只是多建一条连接（任务设计第 6 节）
  if (n > 0) return false;
  return errno == EAGAIN || errno == EWOULDBLOCK;  // 没有数据可读：健康。其他错误（RST 等）：不能用
}

void Connection::close() {
  boost::system::error_code ignored;
  tcp::socket& s = socket();
  if (!s.is_open()) return;
  s.shutdown(tcp::socket::shutdown_both, ignored);
  s.close(ignored);
}

}  // namespace llmgw::upstream::detail
