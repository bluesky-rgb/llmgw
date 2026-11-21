// 协程 demo 的公共定义：参数、统计、命名空间别名。
#pragma once

#include <atomic>
#include <boost/asio.hpp>
#include <boost/asio/experimental/awaitable_operators.hpp>
#include <boost/beast.hpp>
#include <chrono>
#include <cstdint>
#include <string>

namespace spike {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
using tcp = asio::ip::tcp;
using namespace asio::experimental::awaitable_operators;  // 提供 a || b

// 所有异步操作都用这个完成令牌：出错时返回错误码，而不是抛异常
inline constexpr auto kTuple = asio::as_tuple(asio::use_awaitable);

struct Options {
  std::string listen_host = "127.0.0.1";
  std::uint16_t listen_port = 18100;
  std::string upstream_host = "127.0.0.1";
  std::string upstream_port = "18081";
  unsigned workers = 0;  // 0 表示等于 CPU 核数
  std::chrono::seconds total_timeout{600};
  std::chrono::seconds idle_timeout{60};
  bool no_pregrow_fds = false;  // 关闭 fd 表预扩容（只用于对照实验）
};

// demo 中为了简单使用全局原子计数；正式实现中按 Worker 分片，不需要原子操作。
struct Stats {
  std::atomic<std::int64_t> sessions_active{0};
  std::atomic<std::int64_t> requests{0};
  std::atomic<std::int64_t> client_aborted{0};      // 客户端在响应完成前断开
  std::atomic<std::int64_t> upstream_cancelled{0};  // 网关主动取消的上游请求
  std::atomic<std::int64_t> timeouts{0};            // 总超时或空闲超时
  std::atomic<std::int64_t> upstream_errors{0};     // 连接上游失败、上游中途出错
  // 事件循环延迟：每 10ms 的定时器实际晚醒了多久（任务 0.7 加入，用来定位卡顿）
  std::atomic<std::int64_t> loop_lag_max_us{0};
  std::atomic<std::int64_t> loop_lag_over_5ms{0};
  std::atomic<std::int64_t> loop_lag_over_20ms{0};
};

}  // namespace spike
