// 内部：Worker = 一个线程 + 一个只给它自己用的 io_context + 一个 SO_REUSEPORT 的 acceptor。
#pragma once

#include <memory>
#include <optional>
#include <thread>

#include "llmgw/server/server.h"
#include "session.h"

namespace llmgw::server::detail {

class Worker {
 public:
  Worker(unsigned index, std::shared_ptr<const config::Snapshot> config, SharedState* shared,
         const DispatcherFactory& dispatcher_factory, const MetricsFactory& metrics_factory);
  Worker(const Worker&) = delete;
  Worker& operator=(const Worker&) = delete;
  Worker(Worker&&) = delete;
  Worker& operator=(Worker&&) = delete;
  ~Worker();

  /// 打开 acceptor 并绑定（SO_REUSEPORT：所有 Worker 绑定同一个端口，由内核分配新连接）。
  Result<void> listen(const tcp::endpoint& endpoint);
  [[nodiscard]] std::uint16_t port() const;

  void start();  ///< 启动线程
  void stop();   ///< 停止接收、结束事件循环、等线程退出。可以重复调用

 private:
  Task<> accept_loop();

  // 析构顺序很重要：先销毁 io_context（它会销毁所有未完成的 Session 协程，
  // 这些协程中的 ResponseStream 可能引用 Dispatcher 的连接池），再销毁 Dispatcher。
  // 所以 io_context 用 unique_ptr 持有，在析构函数中显式先销毁。
  std::unique_ptr<MetricsRecorder> metrics_;
  std::unique_ptr<Dispatcher> dispatcher_;
  std::unique_ptr<asio::io_context> ioc_;
  std::optional<tcp::acceptor> acceptor_;
  WorkerContext context_;
  std::thread thread_;
};

}  // namespace llmgw::server::detail
