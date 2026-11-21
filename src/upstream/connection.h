// 一条到上游的连接：TCP，或者 TCP（可能经过代理隧道）之上的 TLS。只在一个 Worker 线程上使用。
#pragma once

#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl/stream.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <memory>
#include <string>
#include <utility>

#include "llmgw/core/request_context.h"

namespace llmgw::upstream::detail {

namespace asio = boost::asio;
using tcp = asio::ip::tcp;

/// 响应头的上限，也是读缓冲区的上限（Beast 要求整个响应头在一块连续的缓冲区里）
inline constexpr std::size_t kMaxResponseHeaderBytes = std::size_t{64} * 1024;

class Connection {
 public:
  explicit Connection(tcp::socket socket) : plain_(std::move(socket)) {}
  Connection(const Connection&) = delete;
  Connection& operator=(const Connection&) = delete;
  Connection(Connection&&) = delete;
  Connection& operator=(Connection&&) = delete;
  ~Connection() { close(); }

  /// 在现有的 TCP 连接（可能已经是代理隧道）上建立 TLS。之后所有读写都经过 TLS。
  void start_tls(asio::ssl::context& ctx) {
    tls_ = std::make_unique<asio::ssl::stream<tcp::socket>>(std::move(plain_), ctx);
  }
  [[nodiscard]] bool is_tls() const { return tls_ != nullptr; }
  [[nodiscard]] asio::ssl::stream<tcp::socket>& tls() { return *tls_; }

  /// 对实际的流（TCP 或 TLS）执行 f。两种情况下 f 返回的 awaitable 类型相同。
  template <class F>
  auto with_stream(F&& f) {
    return tls_ ? f(*tls_) : f(plain_);
  }

  tcp::socket& socket() { return tls_ ? tls_->next_layer() : plain_; }
  boost::beast::flat_buffer& buffer() { return buffer_; }

  /// 复用前的检查：连接是否还能用。不阻塞。
  /// 读缓冲区或 TLS 层有未读的数据、上游已经关闭（读到 EOF）、出错，都说明不能复用。
  [[nodiscard]] bool healthy();

  /// 关闭连接（不做 TLS 的 close_notify：对方可能不回应，等待它没有意义）。可以重复调用。
  void close();

  SteadyClock::time_point idle_since;  ///< 放回连接池的时间
  std::uint64_t requests = 0;  ///< 在这条连接上发过的请求数（大于 0 表示是复用的）

 private:
  tcp::socket plain_;
  std::unique_ptr<asio::ssl::stream<tcp::socket>> tls_;
  boost::beast::flat_buffer buffer_{kMaxResponseHeaderBytes};
};

}  // namespace llmgw::upstream::detail
