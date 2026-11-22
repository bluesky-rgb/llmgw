#include "control_plane.h"

#include <cstdio>
#include <exception>
#include <utility>

#include "http_util.h"

namespace llmgw::server::detail {
namespace {

void report(const std::exception_ptr& e) {
  if (!e) return;
  try {
    std::rethrow_exception(e);
  } catch (const std::exception& ex) {
    std::fprintf(stderr, "[server] control plane: %s\n", ex.what());
  }
}

Result<void> open(tcp::acceptor* acceptor, const tcp::endpoint& endpoint, const char* plane) {
  beast::error_code ec;
  acceptor->open(endpoint.protocol(), ec);
  if (!ec) acceptor->set_option(asio::socket_base::reuse_address(true), ec);
  if (!ec) acceptor->bind(endpoint, ec);
  if (!ec) acceptor->listen(asio::socket_base::max_listen_connections, ec);
  if (ec) {
    return fail(ErrorCode::kInternal, std::string(plane) + " 无法监听 " +
                                          endpoint.address().to_string() + ":" +
                                          std::to_string(endpoint.port()) + "：" + ec.message());
  }
  return {};
}

}  // namespace

ControlPlane::ControlPlane(std::shared_ptr<const config::Snapshot> config,
                           const SharedState* shared)
    : config_(std::move(config)), shared_(shared) {}

ControlPlane::~ControlPlane() {
  try {
    stop();
  } catch (const std::exception& e) {  // join() 失败时会抛异常；析构函数不能让异常逃出去
    std::fprintf(stderr, "[server] control plane: stop failed: %s\n", e.what());
  }
}

Result<void> ControlPlane::listen(const tcp::endpoint& admin, const tcp::endpoint& observability) {
  admin_.emplace(ioc_);
  observability_.emplace(ioc_);
  if (auto r = open(&*admin_, admin, "管理面（server.admin_listen）"); !r) return r;
  return open(&*observability_, observability, "观测面（server.observability_listen）");
}

std::uint16_t ControlPlane::admin_port() const {
  return admin_ ? admin_->local_endpoint().port() : 0;
}
std::uint16_t ControlPlane::observability_port() const {
  return observability_ ? observability_->local_endpoint().port() : 0;
}

void ControlPlane::start() {
  if (!admin_ || !observability_) return;  // listen() 没有成功
  asio::co_spawn(ioc_, accept_loop(&admin_.value(), Plane::kAdmin), report);
  asio::co_spawn(ioc_, accept_loop(&observability_.value(), Plane::kObservability), report);
  thread_ = std::thread([this] { ioc_.run(); });
}

void ControlPlane::stop() {
  if (!thread_.joinable()) return;
  asio::post(ioc_, [this] {
    beast::error_code ignored;
    if (admin_) admin_->close(ignored);
    if (observability_) observability_->close(ignored);
    ioc_.stop();
  });
  thread_.join();
}

Task<> ControlPlane::accept_loop(tcp::acceptor* acceptor, Plane plane) {
  asio::steady_timer backoff(ioc_);
  for (;;) {
    auto [ec, socket] = co_await acceptor->async_accept(kTuple);
    if (ec == asio::error::operation_aborted) co_return;
    if (ec) {
      // 比如文件描述符用完：稍等再试。不等的话控制面线程会空转占满一个核（独立审查发现）
      backoff.expires_after(std::chrono::milliseconds(10));
      co_await backoff.async_wait(kTuple);
      continue;
    }
    if (connections_ >= kMaxConnections) continue;  // 超过上限：socket 析构时直接关闭
    ++connections_;
    asio::co_spawn(ioc_, serve(std::move(socket), plane), [this](const std::exception_ptr& e) {
      --connections_;
      report(e);
    });
  }
}

// 每条连接只处理一个请求，然后关闭：探针和 Prometheus 的请求量很小，不需要 Keep-Alive
Task<> ControlPlane::serve(tcp::socket socket, Plane plane) {
  beast::flat_buffer buffer;
  http::request<http::string_body> req;
  asio::steady_timer timer(ioc_);
  timer.expires_after(config_->server.read_timeout);
  auto r = co_await (http::async_read(socket, buffer, req, kTuple) || timer_fired(&timer));
  if (r.index() == 1 || std::get<0>(std::get<0>(r))) co_return;

  http::response<http::string_body> res{http::status::ok, req.version()};
  res.set(http::field::content_type, "application/json");
  res.keep_alive(false);
  const auto path = path_of(req.target());
  auto not_found = [&](std::string message) {
    res.result(http::status::not_found);
    res.body() = error_body(
        Error{.code = ErrorCode::kNotFound, .message = std::move(message), .retry_after = {}});
  };
  if (plane == Plane::kObservability && path == "/health") {
    res.body() = R"({"status":"ok"})";
  } else if (plane == Plane::kObservability && path == "/ready") {
    if (shared_->stopping.load(std::memory_order_relaxed)) {
      res.result(http::status::service_unavailable);
      res.body() = R"({"status":"stopping"})";
    } else {
      res.body() = R"({"status":"ready"})";
    }
  } else if (plane == Plane::kObservability) {
    not_found(path == "/metrics" ? "/metrics 将在任务 2.8 中实现" : "观测面提供 /health 和 /ready");
  } else {
    not_found("管理接口将在 v0.4 中提供（任务 3.x）");
  }
  res.prepare_payload();
  timer.expires_after(config_->server.write_timeout);  // 客户端不读时不能永远占住连接
  co_await (http::async_write(socket, res, kTuple) || timer_fired(&timer));
  beast::error_code ignored;
  socket.shutdown(tcp::socket::shutdown_both, ignored);
}

}  // namespace llmgw::server::detail
