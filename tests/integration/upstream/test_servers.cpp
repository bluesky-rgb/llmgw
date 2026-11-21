#include "test_servers.h"

#include <openssl/ssl.h>
#include <sys/socket.h>

#include <algorithm>
#include <boost/asio/as_tuple.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/experimental/awaitable_operators.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/write.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/http.hpp>
#include <cctype>

#include "tls_certs.h"

namespace llmgw::test {

namespace asio = boost::asio;
namespace http = boost::beast::http;
namespace ssl = asio::ssl;
using tcp = asio::ip::tcp;
using namespace asio::experimental::awaitable_operators;  // NOLINT(google-build-using-namespace)
constexpr auto kTuple = asio::as_tuple(asio::use_awaitable);
template <class T = void>
using Awaitable = asio::awaitable<T>;

std::string Received::header(std::string_view name) const {
  for (const auto& h : headers) {
    if (std::equal(h.name.begin(), h.name.end(), name.begin(), name.end(), [](char a, char b) {
          return std::tolower(static_cast<unsigned char>(a)) ==
                 std::tolower(static_cast<unsigned char>(b));
        })) {
      return h.value;
    }
  }
  return {};
}

Reply ok(const std::string& body, const std::string& extra_headers) {
  return Reply{
      .parts = {Part{{},
                     "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " +
                         std::to_string(body.size()) + "\r\n" + extra_headers + "\r\n" + body}}};
}

Reply chunked(const std::vector<std::string>& pieces, std::chrono::milliseconds gap) {
  Reply r;
  r.parts.push_back(Part{
      {},
      "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nTransfer-Encoding: chunked\r\n\r\n"});
  for (const auto& p : pieces) {
    char size[32];
    std::snprintf(size, sizeof size, "%zx\r\n", p.size());
    r.parts.push_back(Part{gap, size + p + "\r\n"});
  }
  r.parts.push_back(Part{gap, "0\r\n\r\n"});
  return r;
}

namespace {

void rst(tcp::socket& s) {
  boost::system::error_code ignored;
  s.set_option(asio::socket_base::linger(true, 0), ignored);  // close() 时发送 RST
  s.close(ignored);
}

Awaitable<> sleep_for(std::chrono::milliseconds d) {
  if (d.count() <= 0) co_return;
  asio::steady_timer t(co_await asio::this_coro::executor, d);
  co_await t.async_wait(kTuple);
}

}  // namespace

// ---------------- TestUpstream ----------------

struct TestUpstream::Impl {
  TestUpstream* self = nullptr;
  UpstreamOptions options;
  Script script;
  asio::io_context ioc{1};
  tcp::acceptor acceptor{ioc};
  std::unique_ptr<ssl::context> tls;
  mutable std::mutex mu;
  std::vector<Received> received;

  template <class Stream>
  Awaitable<> serve(Stream& stream, tcp::socket& raw, int conn_id) {
    boost::beast::flat_buffer buffer;
    for (int index = 1;; ++index) {
      http::request_parser<http::string_body> parser;
      parser.body_limit(256U << 20);
      // 等下一个请求；设置了 idle_close 时，空闲超时后主动关闭（FIN）
      boost::system::error_code ec;
      if (options.idle_close.count() > 0 && buffer.size() == 0) {
        asio::steady_timer t(ioc, options.idle_close);
        auto r = co_await (raw.async_wait(tcp::socket::wait_read, kTuple) || t.async_wait(kTuple));
        if (r.index() == 1) {
          raw.shutdown(tcp::socket::shutdown_both, ec);
          co_return;
        }
      }
      std::tie(ec, std::ignore) = co_await http::async_read_header(stream, buffer, parser, kTuple);
      if (ec) co_return;
      Received rec;
      rec.connection = conn_id;
      rec.index = index;
      rec.method = std::string(parser.get().method_string());
      rec.target = std::string(parser.get().target());
      for (const auto& f : parser.get())
        rec.headers.push_back({std::string(f.name_string()), std::string(f.value())});
      if (options.rst_before_body && options.rst_before_body(rec)) {
        rst(raw);
        co_return;
      }
      std::tie(ec, std::ignore) = co_await http::async_read(stream, buffer, parser, kTuple);
      if (ec) co_return;
      rec.body = parser.get().body();
      self->requests_.fetch_add(1);
      Reply reply = script(rec);
      {
        const std::lock_guard lock(mu);
        received.push_back(rec);
      }
      if (reply.rst) {
        rst(raw);
        co_return;
      }
      if (reply.hang) {
        asio::steady_timer forever(ioc, std::chrono::hours(1));
        co_await forever.async_wait(kTuple);
        co_return;
      }
      for (const auto& part : reply.parts) {
        co_await sleep_for(part.delay);
        auto [wec, n] = co_await asio::async_write(stream, asio::buffer(part.bytes), kTuple);
        if (wec) co_return;
      }
      if (reply.close_after) {
        raw.shutdown(tcp::socket::shutdown_both, ec);
        co_return;
      }
    }
  }

  Awaitable<> session(tcp::socket socket, int conn_id) {
    boost::system::error_code ignored;
    socket.set_option(tcp::no_delay(true),
                      ignored);  // 否则 TLS 握手的多次小写入会撞上 Nagle + 延迟 ACK（约 40 ms）
    if (!tls) {
      co_await serve(socket, socket, conn_id);
      co_return;
    }
    ssl::stream<tcp::socket> stream(std::move(socket), *tls);
    auto [ec] = co_await stream.async_handshake(ssl::stream_base::server, kTuple);
    if (ec) co_return;
    co_await serve(stream, stream.next_layer(), conn_id);
  }

  Awaitable<> accept_loop() {
    for (;;) {
      auto [ec, socket] = co_await acceptor.async_accept(kTuple);
      if (ec) co_return;
      const int id = self->connections_.fetch_add(1) + 1;
      asio::co_spawn(ioc, session(std::move(socket), id), asio::detached);
    }
  }
};

namespace {

std::unique_ptr<ssl::context> server_tls(TlsMode mode) {
  if (mode == TlsMode::kNone) return nullptr;
  const TestPki& pki = test_pki();
  const CertFiles& files = mode == TlsMode::kWrongHost    ? pki.wrong_host
                           : mode == TlsMode::kExpired    ? pki.expired
                           : mode == TlsMode::kSelfSigned ? pki.self_signed
                                                          : pki.good;
  auto ctx = std::make_unique<ssl::context>(ssl::context::tls_server);
  ctx->use_certificate_chain(asio::buffer(files.pem_cert));
  ctx->use_private_key(asio::buffer(files.pem_key), ssl::context::pem);
  if (mode == TlsMode::kTls11Only) {
    SSL_CTX_set_security_level(ctx->native_handle(), 0);
    SSL_CTX_set_min_proto_version(ctx->native_handle(), TLS1_VERSION);
    SSL_CTX_set_max_proto_version(ctx->native_handle(), TLS1_1_VERSION);
  }
  return ctx;
}

}  // namespace

TestUpstream::TestUpstream(UpstreamOptions options, Script script)
    : impl_(std::make_unique<Impl>()) {
  impl_->self = this;
  impl_->options = std::move(options);
  impl_->script = std::move(script);
  impl_->tls = server_tls(impl_->options.tls);
  const tcp::endpoint ep(asio::ip::make_address("127.0.0.1"), 0);
  impl_->acceptor.open(ep.protocol());
  impl_->acceptor.set_option(tcp::acceptor::reuse_address(true));
  impl_->acceptor.bind(ep);
  impl_->acceptor.listen();
  port_ = impl_->acceptor.local_endpoint().port();
  asio::co_spawn(impl_->ioc, impl_->accept_loop(), asio::detached);
  thread_ = std::thread([this] { impl_->ioc.run(); });
}

TestUpstream::~TestUpstream() {
  impl_->ioc.stop();
  thread_.join();
}

std::vector<Received> TestUpstream::received() const {
  const std::lock_guard lock(impl_->mu);
  return impl_->received;
}

// ---------------- TestProxy ----------------

struct TestProxy::Impl {
  TestProxy* self = nullptr;
  ProxyOptions options;
  asio::io_context ioc{1};
  tcp::acceptor acceptor{ioc};
  mutable std::mutex mu;
  std::string last_target;

  static Awaitable<> copy(tcp::socket& from, tcp::socket& to) {
    std::array<char, 16384> buf{};
    for (;;) {
      auto [ec, n] = co_await from.async_read_some(asio::buffer(buf), kTuple);
      if (ec) co_return;
      auto [wec, wn] = co_await asio::async_write(to, asio::buffer(buf.data(), n), kTuple);
      if (wec) co_return;
    }
  }

  // 把目标 "host:port" 或 "http://host:port/..." 连接到 127.0.0.1:port
  Awaitable<bool> connect_target(tcp::socket& out, std::string_view authority) {
    const auto colon = authority.rfind(':');
    if (colon == std::string_view::npos) co_return false;
    const auto port =
        static_cast<std::uint16_t>(std::stoi(std::string(authority.substr(colon + 1))));
    auto [ec] = co_await out.async_connect(tcp::endpoint(asio::ip::make_address("127.0.0.1"), port),
                                           kTuple);
    co_return !ec;
  }

  Awaitable<> session(tcp::socket client) {
    boost::beast::flat_buffer buffer;
    http::request_parser<http::empty_body> parser;
    parser.skip(true);  // 只读请求头；请求体（如果有）原样转发
    auto [ec, n] = co_await http::async_read_header(client, buffer, parser, kTuple);
    if (ec) co_return;
    const auto& req = parser.get();
    const std::string target(req.target());
    {
      const std::lock_guard lock(mu);
      last_target = target;
    }
    if (!options.user.empty()) {
      const std::string expected = "Basic " + basic(options.user + ":" + options.password);
      if (req[http::field::proxy_authorization] != expected) {
        self->rejected_.fetch_add(1);
        const std::string resp =
            "HTTP/1.1 407 Proxy Authentication Required\r\nProxy-Authenticate: Basic "
            "realm=\"t\"\r\n"
            "Content-Length: 0\r\nConnection: close\r\n\r\n";
        co_await asio::async_write(client, asio::buffer(resp), kTuple);
        co_return;
      }
    }
    tcp::socket upstream(ioc);
    if (req.method() == http::verb::connect) {
      if (!co_await connect_target(upstream, target)) co_return;
      self->tunnels_.fetch_add(1);
      const std::string resp = "HTTP/1.1 200 Connection established\r\n\r\n";
      co_await asio::async_write(client, asio::buffer(resp), kTuple);
    } else {
      // 绝对路径形式：http://host:port/path → 改写成 /path 发给上游
      const auto scheme_end = target.find("://");
      if (scheme_end == std::string::npos) co_return;
      const auto path_start = target.find('/', scheme_end + 3);
      const std::string auth = target.substr(scheme_end + 3, path_start - scheme_end - 3);
      if (!co_await connect_target(upstream, auth)) co_return;
      self->forwards_.fetch_add(1);
      std::string head =
          std::string(req.method_string()) + " " + target.substr(path_start) + " HTTP/1.1\r\n";
      for (const auto& f : req) {
        if (f.name() == http::field::proxy_authorization) continue;
        head += std::string(f.name_string()) + ": " + std::string(f.value()) + "\r\n";
      }
      head += "\r\n";
      co_await asio::async_write(upstream, asio::buffer(head), kTuple);
    }
    if (buffer.size() > 0) {
      co_await asio::async_write(upstream, buffer.data(), kTuple);
      buffer.consume(buffer.size());
    }
    co_await (copy(client, upstream) || copy(upstream, client));
  }

  static std::string basic(const std::string& in) {
    static constexpr std::string_view kAlphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    unsigned val = 0;
    int bits = -6;
    for (const unsigned char c : in) {
      val = (val << 8U) + c;
      bits += 8;
      while (bits >= 0) {
        out += kAlphabet[(val >> static_cast<unsigned>(bits)) & 0x3FU];
        bits -= 6;
      }
    }
    if (bits > -6) out += kAlphabet[((val << 8U) >> static_cast<unsigned>(bits + 8)) & 0x3FU];
    while (out.size() % 4 != 0) out += '=';
    return out;
  }

  Awaitable<> accept_loop() {
    for (;;) {
      auto [ec, socket] = co_await acceptor.async_accept(kTuple);
      if (ec) co_return;
      asio::co_spawn(ioc, session(std::move(socket)), asio::detached);
    }
  }
};

TestProxy::TestProxy(ProxyOptions options) : impl_(std::make_unique<Impl>()) {
  impl_->self = this;
  impl_->options = std::move(options);
  const tcp::endpoint ep(asio::ip::make_address("127.0.0.1"), 0);
  impl_->acceptor.open(ep.protocol());
  impl_->acceptor.set_option(tcp::acceptor::reuse_address(true));
  impl_->acceptor.bind(ep);
  impl_->acceptor.listen();
  port_ = impl_->acceptor.local_endpoint().port();
  asio::co_spawn(impl_->ioc, impl_->accept_loop(), asio::detached);
  thread_ = std::thread([this] { impl_->ioc.run(); });
}

TestProxy::~TestProxy() {
  impl_->ioc.stop();
  thread_.join();
}

std::string TestProxy::last_target() const {
  const std::lock_guard lock(impl_->mu);
  return impl_->last_target;
}

}  // namespace llmgw::test
