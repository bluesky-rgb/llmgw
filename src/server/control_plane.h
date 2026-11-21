// 内部：控制面线程。管理面（默认 127.0.0.1:9090）和观测面（默认
// 0.0.0.0:9100）跑在同一个单独的线程上，
// 数据面再忙也不会挤占健康检查和监控（design.md 3.6“三个平面分开”）。
//
//   观测面：GET /health（存活）、GET /ready（可以接流量；正在停止时 503）；/metrics 在任务 1.2 实现
//   管理面：/admin/* 在任务 3.x 实现，目前一律 404
#pragma once

#include <optional>
#include <string>
#include <thread>

#include "async_util.h"
#include "llmgw/config/snapshot.h"
#include "llmgw/core/result.h"
#include "session.h"

namespace llmgw::server::detail {

class ControlPlane {
 public:
  ControlPlane(std::shared_ptr<const config::Snapshot> config, const SharedState* shared);
  ControlPlane(const ControlPlane&) = delete;
  ControlPlane& operator=(const ControlPlane&) = delete;
  ControlPlane(ControlPlane&&) = delete;
  ControlPlane& operator=(ControlPlane&&) = delete;
  ~ControlPlane();

  Result<void> listen(const tcp::endpoint& admin, const tcp::endpoint& observability);
  [[nodiscard]] std::uint16_t admin_port() const;
  [[nodiscard]] std::uint16_t observability_port() const;
  void start();
  void stop();

 private:
  enum class Plane : std::uint8_t { kAdmin, kObservability };
  Task<> accept_loop(tcp::acceptor* acceptor, Plane plane);
  Task<> serve(tcp::socket socket, Plane plane);

  std::shared_ptr<const config::Snapshot> config_;
  const SharedState* shared_;
  asio::io_context ioc_{1};
  std::optional<tcp::acceptor> admin_;
  std::optional<tcp::acceptor> observability_;
  std::thread thread_;
};

}  // namespace llmgw::server::detail
