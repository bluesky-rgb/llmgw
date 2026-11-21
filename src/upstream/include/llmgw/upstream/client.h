// 到上游的 HTTP/1.1 客户端（任务 1.4）：每个 Worker 一个 Client，内含每个上游一个的连接池、DNS
// 缓存。
//
// 调用方是 provider（1.6）。设计见 docs/tasks/1.4-upstream.md。
#pragma once

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "llmgw/config/snapshot.h"
#include "llmgw/core/error.h"
#include "llmgw/core/metrics.h"
#include "llmgw/core/request_context.h"
#include "llmgw/core/response.h"
#include "llmgw/core/result.h"
#include "llmgw/core/task.h"

namespace llmgw::upstream {

/// 发给上游的请求。Host、Content-Length、Accept-Encoding、Connection 由 Client 设置，
/// 调用者设置的同名请求头会被忽略。
struct Request {
  std::string method = "POST";
  std::string target;  ///< 路径（已经拼好 base_path），比如 "/v1/chat/completions"
  std::vector<Header> headers;  ///< 比如 Authorization、Content-Type
  std::string body;
};

/// 一次尝试的时限（由 executor 按目标的 timeouts 计算）。
struct Deadlines {
  SteadyClock::duration connect;     ///< 排队 + DNS + TCP + 代理握手 + TLS 握手
  SteadyClock::duration first_byte;  ///< 发送请求 + 等待响应头
};

/// 失败发生在哪个阶段。
enum class Stage : std::uint8_t {
  kQueue,     ///< 连接池已满，排队超时
  kResolve,   ///< DNS
  kConnect,   ///< TCP
  kProxy,     ///< 代理握手（CONNECT 被拒绝、认证失败）
  kTls,       ///< TLS 握手、证书校验
  kSend,      ///< 写请求
  kResponse,  ///< 等待或解析响应头
  kBody,      ///< 读响应体
};

std::string_view stage_name(Stage stage);

/// 一次失败。
///
/// 规则（design.md 6.1，任务设计第 6 节）：上游只有收到完整的请求才可能处理它。
/// 写请求失败时 maybe_sent = false；写成功之后的任何失败都是 maybe_sent = true。
struct AttemptError {
  Stage stage = Stage::kConnect;
  /// 上游是否可能已经收到请求（因而可能已经开始计费）。false：可以安全地换一个目标重试
  bool maybe_sent = false;
  bool timed_out = false;
  /// 失败发生在复用的连接上，并且一个字节的响应都没有收到。上游恰好关闭空闲连接时会这样，
  /// 1.7 可以据此决定是否允许重试（默认不重试，见任务设计第 6 节最后一段）
  bool reused_without_response = false;
  std::string detail;  ///< 给日志看；不包含密钥、代理密码

  /// 转换成返回给客户端的错误：排队超时 → 503；没发出去 → 502；可能已发出 → 504（不建议重试）
  [[nodiscard]] Error to_error() const;
};

template <typename T>
using Outcome = Expected<T, AttemptError>;

/// 响应体读取器。只在创建它的 Worker 线程上使用。
///
/// 析构时：响应体已经完整读完、上游允许复用、读缓冲区里没有多余的字节 → 把连接还回连接池；
/// 其他任何情况（没读完、出错、被取消、上游要求关闭）→ 关闭连接。
class Body {
 public:
  Body() = default;
  Body(const Body&) = delete;
  Body& operator=(const Body&) = delete;
  Body(Body&&) = delete;
  Body& operator=(Body&&) = delete;
  virtual ~Body() = default;

  /// 把下一段数据（已经去掉分块编码）追加到 out，不清空 out。
  /// true：读到了数据；false：响应体正常结束，之后不要再调用；
  /// AttemptError：中途出错（stage = kBody，maybe_sent = true），之后不要再调用
  [[nodiscard]] virtual Task<Outcome<bool>> read(std::string* out) = 0;
};

/// 已经读完响应头的响应。
struct Response {
  int status = 0;
  std::vector<Header> headers;
  std::unique_ptr<Body> body;
};

/// 全局共享的 TLS 上下文（每种 CA 设置一个 SSL_CTX）。启动时创建，之后只读，可以跨线程共享。
class TlsContexts;

/// 为配置中每一种 CA 设置创建 TLS 上下文。加载系统证书库约 40 ms，所以只在启动时调用一次。
/// CA 文件无法加载时返回错误（说明是哪个上游、哪个文件）。
Result<std::shared_ptr<const TlsContexts>> make_tls_contexts(const config::Snapshot& snapshot);

/// 每个 Worker 到一个上游的连接上限：⌈总数 ÷ Worker 数⌉，至少为 1。
std::size_t per_worker_limit(std::uint32_t total, unsigned worker_count);

/// DNS 解析函数：主机名 + 端口 → 地址列表。测试中注入，以统计次数、制造失败。
using Resolver = std::function<Task<Result<std::vector<boost::asio::ip::tcp::endpoint>>>(
    std::string host, std::uint16_t port)>;

struct ClientOptions {
  boost::asio::any_io_executor executor;           ///< 这个 Worker 的事件循环
  std::shared_ptr<const config::Snapshot> config;  ///< 上游、代理、连接池配置
  std::shared_ptr<const TlsContexts> tls;          ///< make_tls_contexts() 的结果
  unsigned worker_count = 1;                       ///< 用来计算每个 Worker 的连接上限
  /// 为空时不上报。不为空时必须比 executor 所属的 io_context 活得久：io_context 销毁时，
  /// 挂起的请求的协程帧被销毁，归还名额时仍会上报连接池的指标
  MetricsRecorder* metrics = nullptr;
  Resolver resolver;  ///< 为空时用 Asio 的 async_resolve
  SteadyClock::duration dns_ttl = std::chrono::seconds(60);
  SteadyClock::duration sweep_interval = std::chrono::seconds(10);  ///< 空闲连接的清理周期
};

/// 每个 Worker 一个。只在 options.executor 对应的线程上使用。
class Client {
 public:
  explicit Client(ClientOptions options);
  ~Client();
  Client(const Client&) = delete;
  Client& operator=(const Client&) = delete;
  Client(Client&&) = delete;
  Client& operator=(Client&&) = delete;

  /// 发送请求，等到响应头（跳过 1xx）。provider 必须指向 options.config 中的上游（不能为空；
  /// 用指针而不是引用：协程会在调用者的表达式结束后继续执行，引用参数容易绑定到临时对象）。
  /// 被取消（客户端断开、总超时）时尽快返回一个 AttemptError，连接不会还回连接池。
  /// 不抛异常（可以直接放进竞速，ADR-006 补充规则 7）。
  [[nodiscard]] Task<Outcome<Response>> send(const config::Provider* provider, Request request,
                                             Deadlines deadlines);

  /// 当前的空闲连接数、占用的连接数（测试与指标用）。
  struct PoolStats {
    std::size_t idle = 0;
    std::size_t busy = 0;     ///< 借出的 + 正在建立的
    std::size_t waiting = 0;  ///< 排队的请求数
  };
  [[nodiscard]] PoolStats stats(const std::string& provider) const;

 private:
  struct State;
  static Task<> sweep_loop(std::weak_ptr<State> weak);
  std::shared_ptr<State> state_;
};

}  // namespace llmgw::upstream
