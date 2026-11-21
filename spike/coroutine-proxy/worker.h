// Worker：一个线程 + 一个只给它自己用的 io_context + 一个 SO_REUSEPORT 的 acceptor。
// Worker 之间不共享任何会话状态，所以会话内部不需要加锁。
#pragma once

#include <cstdio>
#include <exception>
#include <memory>
#include <thread>

#include "common.h"
#include "session.h"

namespace spike {

// SO_REUSEPORT：多个 acceptor 绑定同一个端口，由内核把新连接分给它们
using ReusePort = asio::detail::socket_option::boolean<SOL_SOCKET, SO_REUSEPORT>;

class Worker {
 public:
  Worker(const Options& opt, tcp::resolver::results_type upstream, Stats* stats)
      : ioc_(1), acceptor_(ioc_), opt_(opt), upstream_(std::move(upstream)), stats_(stats) {
    tcp::endpoint ep{asio::ip::make_address(opt.listen_host), opt.listen_port};
    acceptor_.open(ep.protocol());
    acceptor_.set_option(asio::socket_base::reuse_address(true));
    acceptor_.set_option(ReusePort(true));
    acceptor_.bind(ep);
    acceptor_.listen(4096);
  }

  void start() {
    asio::co_spawn(ioc_, accept_loop(), [](std::exception_ptr e) { report(e, "accept loop"); });
    asio::co_spawn(ioc_, lag_probe(), [](std::exception_ptr e) { report(e, "lag probe"); });
    thread_ = std::thread([this] { ioc_.run(); });
  }

  void stop() {
    ioc_.stop();
    if (thread_.joinable()) thread_.join();
  }

 private:
  asio::awaitable<void> accept_loop() {
    for (;;) {
      auto [ec, sock] = co_await acceptor_.async_accept(kTuple);
      if (ec) {
        if (ec == asio::error::operation_aborted) co_return;
        continue;  // 比如文件描述符暂时用完：跳过这一次，不退出
      }
      sock.set_option(tcp::no_delay(true));
      auto session = std::make_shared<Session>(std::move(sock), opt_, upstream_, stats_);
      // 每条连接一个协程；会话出现异常只影响它自己（异常隔离）
      asio::co_spawn(
          ioc_, [session]() -> asio::awaitable<void> { co_await session->run(); },
          [](std::exception_ptr e) { report(e, "session"); });
    }
  }

  // 事件循环延迟探针：如果有同步操作卡住了线程，定时器就会晚醒
  asio::awaitable<void> lag_probe() {
    using Clock = std::chrono::steady_clock;
    asio::steady_timer t(ioc_);
    const auto start = Clock::now();
    for (;;) {
      const auto due = Clock::now() + std::chrono::milliseconds(10);
      t.expires_at(due);
      if (auto [ec] = co_await t.async_wait(kTuple); ec) co_return;
      const auto lag =
          std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - due).count();
      auto cur = stats_->loop_lag_max_us.load();
      while (lag > cur && !stats_->loop_lag_max_us.compare_exchange_weak(cur, lag)) {
      }
      if (lag > 5000) {
        ++stats_->loop_lag_over_5ms;
        std::fprintf(stderr, "[lag] t=%.3fs lag=%.1fms sessions=%lld requests=%lld\n",
                     std::chrono::duration<double>(Clock::now() - start).count(), lag / 1000.0,
                     static_cast<long long>(stats_->sessions_active.load()),
                     static_cast<long long>(stats_->requests.load()));
      }
      if (lag > 20000) ++stats_->loop_lag_over_20ms;
    }
  }

  static void report(std::exception_ptr e, const char* where) {
    if (!e) return;
    try {
      std::rethrow_exception(e);
    } catch (const std::exception& ex) {
      std::fprintf(stderr, "[%s] exception: %s\n", where, ex.what());
    }
  }

  asio::io_context ioc_;
  tcp::acceptor acceptor_;
  Options opt_;
  tcp::resolver::results_type upstream_;
  Stats* stats_;
  std::thread thread_;
};

}  // namespace spike
