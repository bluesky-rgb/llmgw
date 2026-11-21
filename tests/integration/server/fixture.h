// server 集成测试的公共部分：FakeDispatcher（按模型名决定行为）和一个同步的 HTTP 测试客户端。
// 在进程内启动 Server（端口 0，由系统分配），不需要任何网络上游。
#pragma once

#include <gtest/gtest.h>

#include <atomic>
#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <chrono>
#include <memory>
#include <string>
#include <thread>

#include "llmgw/config/snapshot.h"
#include "llmgw/server/server.h"

namespace llmgw::server::test {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
using tcp = asio::ip::tcp;
using Clock = std::chrono::steady_clock;
constexpr auto kTuple = asio::as_tuple(asio::use_awaitable);

/// FakeDispatcher 的记录。Dispatcher 在 Worker 线程上运行、测试在测试线程上读取，所以都是原子变量。
struct Probe {
  std::atomic<int> dispatches{0};
  std::atomic<int> dispatch_cancelled{0};
  std::atomic<int> streams_destroyed{0};
  std::atomic<int> chunks_served{0};
  std::atomic<Clock::rep> last_cancel{0};  ///< 最近一次被取消（或流被销毁）的时间
  void mark_cancel() { last_cancel = Clock::now().time_since_epoch().count(); }
  [[nodiscard]] Clock::time_point cancel_time() const {
    return Clock::time_point(Clock::duration(last_cancel.load()));
  }
};

/// 按请求中的模型名决定行为：
///   ok          立即返回 3 块            slow     每 50 ms 一块，共 1000 块
///   hang        dispatch() 一直不返回    stall    1 块之后一直不再发送
///   midfail     2 块之后出错             error:N  返回 ErrorCode N（带 Retry-After: 2）
///   throw       dispatch() 抛出异常      worker   响应头中带上 Worker 编号
///   big         64 KB × 200 块（背压）  medium   每 50 ms 一块，共 5 块
///   nextthrow   1 块之后 next() 抛异常   badstatus 状态码 1000（不合法）
class FakeDispatcher final : public Dispatcher {
 public:
  FakeDispatcher(WorkerInfo info, std::shared_ptr<Probe> probe)
      : info_(std::move(info)), probe_(std::move(probe)) {}
  Task<Result<std::unique_ptr<ResponseStream>>> dispatch(RequestContext* ctx) override;

 private:
  WorkerInfo info_;
  std::shared_ptr<Probe> probe_;
};

/// 测试用的配置：端口都是 0；超时设得很短，让测试快速运行。
std::shared_ptr<config::Snapshot> test_config();

/// 启动一个 Server 的测试夹具。
class ServerTest : public ::testing::Test {
 protected:
  void SetUp() override { start(test_config()); }
  void TearDown() override { server_.reset(); }
  void start(std::shared_ptr<config::Snapshot> cfg);

  std::shared_ptr<Probe> probe_ = std::make_shared<Probe>();
  std::unique_ptr<Server> server_;
};

/// 测试客户端。每个操作都有超时：内部用异步接口 + io_context::run_for
/// 执行，超时没完成就取消并报告失败， 测试不会因为服务端的 bug 永远卡住。（Asio 的同步读在 EAGAIN
/// 后会无限期 poll，SO_RCVTIMEO 不起作用。）
class Client {
 public:
  explicit Client(std::uint16_t port,
                  std::chrono::milliseconds timeout = std::chrono::milliseconds(5000));
  void send(const std::string& raw);
  /// 不做断言的发送：服务端可能已经关闭了连接（测试慢速攻击、客户端不读等场景时使用）
  bool try_send(const std::string& raw);
  /// 读最多 max 字节（先取缓冲区中已有的）；超时、出错或连接关闭时返回空
  std::string read_some(std::size_t max);
  /// 读到连接关闭或超过 within 为止，返回读到的全部数据
  std::string read_until_close_or(std::chrono::milliseconds within);
  /// 发一个 POST /v1/chat/completions，body 为 {"model": model, "stream": stream}
  void post_chat(const std::string& model, bool stream = true,
                 const std::string& extra_headers = "");
  /// 读一个完整的响应（自动处理分块传输）。超时或出错时 ok 为 false
  struct Response {
    bool ok = false;
    http::response<http::string_body> msg;
  };
  Response read_response();
  /// 只读响应头
  bool read_head(http::response_parser<http::string_body>* parser);
  /// 读到连接关闭为止，返回读到的全部数据（包括缓冲区中已有的）
  std::string read_until_close();
  /// 服务端是否在 within 时间内关闭了连接
  bool closed_within(std::chrono::milliseconds within);
  void close();

 private:
  // 执行一个异步操作，最多等待 timeout；返回操作的错误码（超时为 timed_out）
  template <class Start>
  beast::error_code run(Start start, std::chrono::milliseconds timeout);

  asio::io_context ioc_;
  tcp::socket socket_;
  beast::flat_buffer buffer_;
  std::chrono::milliseconds timeout_;
};

}  // namespace llmgw::server::test
