// Server：接入层（任务 1.3）。server 模块唯一的公开类；Worker、Session、控制面都是内部实现。
//
// 线程：N 个 Worker 线程（数据面，每个线程一个 io_context + 一个 SO_REUSEPORT 的 acceptor），
// 加 1 个控制面线程（管理面、观测面）。设计见 docs/tasks/1.3-server.md。
#pragma once

#include <boost/asio/any_io_executor.hpp>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>

#include "llmgw/config/snapshot.h"
#include "llmgw/core/metrics.h"
#include "llmgw/core/result.h"
#include "llmgw/middleware/chain.h"
#include "llmgw/server/dispatcher.h"

namespace llmgw::server {

/// 一个 Worker 的信息，传给各种工厂函数。
struct WorkerInfo {
  unsigned index = 0;  ///< 0 … workers-1
  unsigned worker_count = 1;  ///< Worker 的总数（每个 Worker 的连接池上限 = 总上限 ÷ Worker 数）
  boost::asio::any_io_executor
      executor;  ///< 这个 Worker 的事件循环（Dispatcher 的实现用它建立上游连接）
};

using DispatcherFactory = std::function<std::unique_ptr<Dispatcher>(const WorkerInfo&)>;
using MetricsFactory = std::function<std::unique_ptr<MetricsRecorder>(const WorkerInfo&)>;
/// 为每个 Worker 创建一条中间件链（任务 2.1）。metrics 是这个 Worker 的指标实现，生命周期长于链。
/// 返回错误时 Server::start() 失败。
using ChainFactory = std::function<Result<std::unique_ptr<middleware::Chain>>(
    const WorkerInfo&, MetricsRecorder* metrics)>;

struct ServerOptions {
  /// 监听地址、上限、超时都从这里读取。端口写 0 时由系统分配（测试使用）。
  std::shared_ptr<const config::Snapshot> config;
  /// 为每个 Worker 创建一个 Dispatcher。必填。
  DispatcherFactory dispatcher_factory;
  /// 为每个 Worker 创建一个 MetricsRecorder。可选，默认 NullMetricsRecorder。
  MetricsFactory metrics_factory;
  /// 为每个 Worker 创建中间件链。可选，默认没有中间件。
  ChainFactory chain_factory;
  /// 整个 on_complete 阶段（所有插件加起来）的时限：超过时取消、关闭连接（middleware.h 中的约定）。
  /// 只有测试需要改它
  std::chrono::milliseconds middleware_complete_timeout{5000};
};

class Server {
 public:
  explicit Server(ServerOptions options);
  Server(const Server&) = delete;
  Server& operator=(const Server&) = delete;
  Server(Server&&) = delete;
  Server& operator=(Server&&) = delete;
  ~Server();  ///< 等同于 stop()

  /// 绑定端口，启动 Worker 和控制面线程。端口被占用等错误在这里返回（说明是哪个端口），
  /// 失败时释放已经绑定的端口。只能调用一次。
  [[nodiscard]] Result<void> start();

  /// 停止：停止接收新连接，关闭所有连接，等所有线程退出。可以重复调用。
  /// 注意：目前 stop() 会立即关闭控制面，“/ready 返回 503 让负载均衡器摘掉流量”的窗口只有微秒级；
  /// 先返回 503、等进行中的请求结束再退出的“优雅退出”在任务 3.x 中实现。
  void stop();

  /// 实际监听的端口。start() 成功之后才有意义。
  [[nodiscard]] std::uint16_t data_port() const;
  [[nodiscard]] std::uint16_t admin_port() const;
  [[nodiscard]] std::uint16_t observability_port() const;
  /// Worker 的个数（配置为 0 时等于 CPU 核数）。
  [[nodiscard]] unsigned workers() const;

 private:
  Result<void> start_impl();

  struct Impl;
  std::unique_ptr<Impl> impl_;
};

/// 监听地址 host 解析出来的地址是不是本机地址（127.0.0.0/8、::1，以及 IPv4 映射的 ::ffff:127.x）。
/// 解析方式与 Server::start 绑定时相同（被动解析，取第一个结果），所以判断的就是实际要绑定的地址：
/// 只看字符串时，主机名 127.evil.example 可以被 /etc/hosts 解析成 0.0.0.0（任务 1.9 独立审查 #1）。
/// 解析失败时返回错误。只在启动时调用（会阻塞）。
[[nodiscard]] Result<bool> listen_address_is_loopback(const std::string& host);

/// 把进程的文件描述符表预扩容到 RLIMIT_NOFILE。
/// 必须在创建任何线程之前调用：多线程进程每次扩容 fd 表都要等一个 RCU 宽限期，
/// 建连高峰期会让事件循环卡顿约 10 ms（任务 0.7 发现，ADR-006 结论 4）。
void pregrow_fd_table();

}  // namespace llmgw::server
