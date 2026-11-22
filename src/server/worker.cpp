#include "worker.h"

#include <cstdio>
#include <exception>
#include <utility>

namespace llmgw::server::detail {
namespace {

using ReusePort = asio::detail::socket_option::boolean<SOL_SOCKET, SO_REUSEPORT>;
constexpr int kListenBacklog = 4096;

void report(const std::exception_ptr& e, const char* where) {
  if (!e) return;
  try {
    std::rethrow_exception(e);
  } catch (const std::exception& ex) {
    std::fprintf(stderr, "[server] %s: %s\n", where, ex.what());
  }
}

}  // namespace

Worker::Worker(unsigned index, unsigned worker_count,
               std::shared_ptr<const config::Snapshot> config, SharedState* shared,
               const ServerOptions& options)
    : ioc_(std::make_unique<asio::io_context>(1)) {
  const WorkerInfo info{
      .index = index, .worker_count = worker_count, .executor = ioc_->get_executor()};
  dispatcher_ = options.dispatcher_factory(info);
  metrics_ = options.metrics_factory ? options.metrics_factory(info) : nullptr;
  if (!metrics_) metrics_ = std::make_unique<NullMetricsRecorder>();
  if (options.chain_factory) {
    auto chain = options.chain_factory(info, metrics_.get());
    if (chain) {
      chain_ = std::move(*chain);
    } else {
      init_error_ = chain.error();
    }
  }
  // 空的链等同于没有链：Session 不需要为它做任何事（热路径上没有额外开销）
  context_.chain = chain_ != nullptr && !chain_->empty() ? chain_.get() : nullptr;
  context_.complete_timeout = options.middleware_complete_timeout;
  context_.index = index;
  context_.config = std::move(config);
  context_.dispatcher = dispatcher_.get();
  context_.metrics = metrics_.get();
  context_.shared = shared;
  context_.rng.seed(std::random_device{}() ^ (std::uint64_t{index} << 32U));
}

Worker::~Worker() {
  try {
    stop();
  } catch (const std::exception& e) {  // join() 失败时会抛异常；析构函数不能让异常逃出去
    std::fprintf(stderr, "[server] worker %u: stop failed: %s\n", context_.index, e.what());
  }
  acceptor_.reset();
  dispatcher_.reset();  // 先于 io_context 销毁，见头文件中的说明
  chain_.reset();
  ioc_.reset();
}

Result<void> Worker::listen(const tcp::endpoint& endpoint) {
  beast::error_code ec;
  acceptor_.emplace(*ioc_);
  acceptor_->open(endpoint.protocol(), ec);
  if (!ec) acceptor_->set_option(asio::socket_base::reuse_address(true), ec);
  if (!ec) acceptor_->set_option(ReusePort(true), ec);
  if (!ec) acceptor_->bind(endpoint, ec);
  if (!ec) acceptor_->listen(kListenBacklog, ec);
  if (ec) return fail(ErrorCode::kInternal, ec.message());
  return {};
}

std::uint16_t Worker::port() const { return acceptor_ ? acceptor_->local_endpoint().port() : 0; }

void Worker::start() {
  asio::co_spawn(*ioc_, accept_loop(),
                 [](const std::exception_ptr& e) { report(e, "accept loop"); });
  thread_ = std::thread([this] { ioc_->run(); });
}

void Worker::stop() {
  if (!thread_.joinable()) return;
  asio::post(*ioc_, [this] {
    beast::error_code ignored;
    if (acceptor_) acceptor_->close(ignored);
    ioc_->stop();
  });
  thread_.join();
}

Task<> Worker::accept_loop() {
  if (!acceptor_) co_return;
  tcp::acceptor& acceptor = *acceptor_;
  const auto max = static_cast<std::int64_t>(context_.config->server.max_connections);
  asio::steady_timer backoff(*ioc_);
  for (;;) {
    auto [ec, socket] = co_await acceptor.async_accept(kTuple);
    if (ec == asio::error::operation_aborted) co_return;
    if (ec) {
      // 比如文件描述符暂时用完：稍等再试，避免空转占满 CPU
      backoff.expires_after(std::chrono::milliseconds(10));
      co_await backoff.async_wait(kTuple);
      continue;
    }
    beast::error_code ignored;
    socket.set_option(tcp::no_delay(true), ignored);
    const auto n = context_.shared->connections.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n > max) {
      asio::co_spawn(*ioc_, reject_overloaded(std::move(socket), &context_),
                     [](const std::exception_ptr& e) { report(e, "reject"); });
      continue;
    }
    // 每条连接一个协程；会话出现异常只影响它自己（ConnectionGuard 保证计数正确）
    asio::co_spawn(*ioc_, run_session(std::make_unique<Session>(std::move(socket), &context_)),
                   [](const std::exception_ptr& e) { report(e, "session"); });
  }
}

}  // namespace llmgw::server::detail
