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
  Worker(unsigned index, unsigned worker_count, std::shared_ptr<const config::Snapshot> config,
         SharedState* shared, const ServerOptions& options);
  Worker(const Worker&) = delete;
  Worker& operator=(const Worker&) = delete;
  Worker(Worker&&) = delete;
  Worker& operator=(Worker&&) = delete;
  ~Worker();

  /// 打开 acceptor 并绑定（SO_REUSEPORT：所有 Worker 绑定同一个端口，由内核分配新连接）。
  Result<void> listen(const tcp::endpoint& endpoint);
  [[nodiscard]] std::uint16_t port() const;
  /// 创建中间件链失败时的错误（构造函数不能返回错误）。
  [[nodiscard]] const std::optional<Error>& init_error() const { return init_error_; }

  void start();  ///< 启动线程
  void stop();   ///< 停止接收、结束事件循环、等线程退出。可以重复调用

 private:
  Task<> accept_loop();

  // 析构顺序很重要（析构函数中显式控制）：
  // 1. 先销毁 Dispatcher：Gateway 中的 upstream::Client 持有定时器、空闲连接等 I/O 对象，
  //    析构时要用到 io_context 的服务，必须在 io_context 之前销毁（独立审查 1.7 #1：顺序反了时
  //    ASan 报 heap-use-after-free）
  // 2. 再销毁 io_context：它销毁所有未完成的 Session 协程。这些协程中的 ResponseStream、Body 通过
  //    shared_ptr 持有连接池的状态，不依赖 Client 对象本身（任务 1.4 U10 验证过 Body 比 Client
  //    活得久）
  // 所以 Dispatcher 的实现不能让未完成的协程在析构时访问 Dispatcher 对象本身。
  // 中间件链与 Dispatcher 一样（插件也可能持有 I/O 对象），在 io_context 之前销毁；
  // 两者都用到 metrics_，所以 metrics_ 最后销毁。
  std::unique_ptr<MetricsRecorder> metrics_;
  std::unique_ptr<Dispatcher> dispatcher_;
  std::unique_ptr<middleware::Chain> chain_;
  std::optional<Error> init_error_;
  std::unique_ptr<asio::io_context> ioc_;
  std::optional<tcp::acceptor> acceptor_;
  WorkerContext context_;
  std::thread thread_;
};

}  // namespace llmgw::server::detail
