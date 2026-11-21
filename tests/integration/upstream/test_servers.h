// upstream 测试用的服务器：可编程的上游、HTTP 代理。各自运行在单独的线程上。
#pragma once

#include <atomic>
#include <boost/asio/io_context.hpp>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "llmgw/core/response.h"

namespace llmgw::test {

/// 上游收到的一个请求。
struct Received {
  int connection = 0;  ///< 第几条连接（从 1 开始）
  int index = 0;       ///< 这条连接上的第几个请求（从 1 开始）
  std::string method;
  std::string target;
  std::vector<Header> headers;
  std::string body;
  [[nodiscard]] std::string header(std::string_view name) const;  ///< 不区分大小写；没有时为空
};

/// 上游的一段输出：先等 delay，再写 bytes。
struct Part {
  std::chrono::milliseconds delay{0};
  std::string bytes;
};

/// 上游对一个请求的处理。
struct Reply {
  std::vector<Part> parts;  ///< 依次写出的原始字节（状态行、响应头、响应体都由测试自己写）
  bool close_after = false;  ///< 写完后关闭连接
  bool rst = false;          ///< 不回复，直接 RST
  bool hang = false;         ///< 不回复，连接一直保持到服务器停止
};

/// 常用的响应
Reply ok(const std::string& body, const std::string& extra_headers = "");
Reply chunked(const std::vector<std::string>& pieces, std::chrono::milliseconds gap = {});

enum class TlsMode : std::uint8_t { kNone, kGood, kWrongHost, kExpired, kSelfSigned, kTls11Only };

struct UpstreamOptions {
  TlsMode tls = TlsMode::kNone;
  /// 大于 0：连接空闲这么久之后服务端主动关闭（FIN），模拟上游的 keep-alive 超时
  std::chrono::milliseconds idle_close{0};
  /// 读完请求头之后调用：返回 true 时不读请求体，直接 RST（模拟上游在请求到达前关闭了连接）
  std::function<bool(const Received&)> rst_before_body;
};

using Script = std::function<Reply(const Received&)>;

class TestUpstream {
 public:
  TestUpstream(UpstreamOptions options, Script script);
  explicit TestUpstream(Script script) : TestUpstream(UpstreamOptions{}, std::move(script)) {}
  ~TestUpstream();
  TestUpstream(const TestUpstream&) = delete;
  TestUpstream& operator=(const TestUpstream&) = delete;
  TestUpstream(TestUpstream&&) = delete;
  TestUpstream& operator=(TestUpstream&&) = delete;

  [[nodiscard]] std::uint16_t port() const { return port_; }
  [[nodiscard]] int connections() const { return connections_.load(); }
  [[nodiscard]] int requests() const { return requests_.load(); }
  [[nodiscard]] std::vector<Received> received() const;

  struct Impl;

 private:
  std::unique_ptr<Impl> impl_;
  std::uint16_t port_ = 0;
  std::atomic<int> connections_{0};
  std::atomic<int> requests_{0};
  std::thread thread_;
};

struct ProxyOptions {
  std::string user;  ///< 为空表示不要求认证
  std::string password;
};

/// HTTP 代理：CONNECT 隧道，以及绝对路径形式的转发。所有主机名都解析为 127.0.0.1。
class TestProxy {
 public:
  explicit TestProxy(ProxyOptions options = {});
  ~TestProxy();
  TestProxy(const TestProxy&) = delete;
  TestProxy& operator=(const TestProxy&) = delete;
  TestProxy(TestProxy&&) = delete;
  TestProxy& operator=(TestProxy&&) = delete;

  [[nodiscard]] std::uint16_t port() const { return port_; }
  [[nodiscard]] int tunnels() const { return tunnels_.load(); }  ///< CONNECT 成功的次数
  [[nodiscard]] int forwards() const { return forwards_.load(); }  ///< 绝对路径形式转发的次数
  [[nodiscard]] int rejected() const { return rejected_.load(); }  ///< 认证失败（407）的次数
  [[nodiscard]] std::string last_target() const;                   ///< 最近一个请求的目标

  struct Impl;

 private:
  std::unique_ptr<Impl> impl_;
  std::uint16_t port_ = 0;
  std::atomic<int> tunnels_{0};
  std::atomic<int> forwards_{0};
  std::atomic<int> rejected_{0};
  std::thread thread_;
};

}  // namespace llmgw::test
