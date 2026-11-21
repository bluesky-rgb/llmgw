// Server：接入层（任务 1.3）。server 模块唯一的公开类；Worker、Session、控制面都是内部实现。
//
// 线程：N 个 Worker 线程（数据面，每个线程一个 io_context + 一个 SO_REUSEPORT 的 acceptor），
// 加 1 个控制面线程（管理面、观测面）。设计见 docs/tasks/1.3-server.md。
#pragma once

#include <boost/asio/any_io_executor.hpp>
#include <cstdint>
#include <functional>
#include <memory>

#include "llmgw/config/snapshot.h"
#include "llmgw/core/metrics.h"
#include "llmgw/core/result.h"
#include "llmgw/server/dispatcher.h"

namespace llmgw::server {

/// 一个 Worker 的信息，传给各种工厂函数。
struct WorkerInfo {
  unsigned index = 0;  ///< 0 … workers-1
  boost::asio::any_io_executor
      executor;  ///< 这个 Worker 的事件循环（Dispatcher 的实现用它建立上游连接）
};

using DispatcherFactory = std::function<std::unique_ptr<Dispatcher>(const WorkerInfo&)>;
using MetricsFactory = std::function<std::unique_ptr<MetricsRecorder>(const WorkerInfo&)>;

struct ServerOptions {
  /// 监听地址、上限、超时都从这里读取。端口写 0 时由系统分配（测试使用）。
  std::shared_ptr<const config::Snapshot> config;
  /// 为每个 Worker 创建一个 Dispatcher。必填。
  DispatcherFactory dispatcher_factory;
  /// 为每个 Worker 创建一个 MetricsRecorder。可选，默认 NullMetricsRecorder。
  MetricsFactory metrics_factory;
};

class Server {
 public:
  explicit Server(ServerOptions options);
  Server(const Server&) = delete;
  Server& operator=(const Server&) = delete;
  Server(Server&&) = delete;
  Server& operator=(Server&&) = delete;
  ~Server();  ///< 等同于 stop()

  /// 绑定端口，启动 Worker 和控制面线程。端口被占用等错误在这里返回（说明是哪个端口）。
  [[nodiscard]] Result<void> start();

  /// 停止：/ready 立即返回 503，停止接收新连接，关闭所有连接，等所有线程退出。
  /// 可以重复调用。（等进行中的请求结束的“优雅退出”在任务 3.x 中实现。）
  void stop();

  /// 实际监听的端口。start() 成功之后才有意义。
  [[nodiscard]] std::uint16_t data_port() const;
  [[nodiscard]] std::uint16_t admin_port() const;
  [[nodiscard]] std::uint16_t observability_port() const;
  /// Worker 的个数（配置为 0 时等于 CPU 核数）。
  [[nodiscard]] unsigned workers() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

/// 把进程的文件描述符表预扩容到 RLIMIT_NOFILE。
/// 必须在创建任何线程之前调用：多线程进程每次扩容 fd 表都要等一个 RCU 宽限期，
/// 建连高峰期会让事件循环卡顿约 10 ms（任务 0.7 发现，ADR-006 结论 4）。
void pregrow_fd_table();

}  // namespace llmgw::server
