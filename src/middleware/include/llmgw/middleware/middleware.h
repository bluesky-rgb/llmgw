// 中间件（扩展点之一，extensibility.md 第 3、4 节）：Middleware、MiddlewareFactory
// 的接口（任务 2.1）。
//
// Session 在 dispatch 之前依次调用各插件的 on_request，请求结束后按相反的顺序调用 on_complete
// （design.md 3.2 的 ② ⑩）。插件之间、插件与 router / executor 之间互不调用，只通过 RequestContext
// 传递数据，每个字段只有一个写入者（比如 auth 插件写 ctx->auth）。
#pragma once

#include <boost/asio/any_io_executor.hpp>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "llmgw/config/loader.h"
#include "llmgw/config/snapshot.h"
#include "llmgw/core/error.h"
#include "llmgw/core/metrics.h"
#include "llmgw/core/request_context.h"
#include "llmgw/core/response.h"
#include "llmgw/core/task.h"

namespace llmgw::middleware {

/// on_request 的结果：放行，或者拒绝。两种情况都可以给响应附加响应头（比如限流的额度响应头）。
/// 以后“插件直接给出完整响应”（缓存命中）也放在这里，现有的插件不受影响。
struct Decision {
  /// 有值：拒绝。返回这个错误对应的状态码和错误体，之后的插件和 dispatch 都不再执行
  std::optional<Error> reject;
  /// 附加到响应上的响应头（成功和拒绝时都加）。会替换上游的同名响应头；同一个名字可以出现多次。
  /// 名字和值必须是合法的 HTTP 头（不含 CR/LF），不能是
  /// Content-Length、Content-Type、X-Request-Id、 Retry-After 等由网关决定的头（Session
  /// 会丢弃这些；要给出 Retry-After 用 Error::retry_after）
  std::vector<Header> response_headers;

  static Decision pass() { return {}; }
  static Decision deny(Error error) {
    return Decision{.reject = std::move(error), .response_headers = {}};
  }
};

/// 插件实例：每个 Worker 一个（MiddlewareFactory::create），只在该 Worker 的线程上被调用，
/// 内部不需要加锁。需要跨 Worker 共享的数据（比如预算账本、并发计数）由工厂在 create 时注入
/// （shared_ptr 持有、自己保证线程安全的对象），不能用全局变量（ADR-009）。
///
/// 生命周期：Worker 停止时，插件实例先于事件循环销毁；还没有结束的 on_request / on_complete 协程帧
/// 在那之后才随事件循环一起销毁（与 Dispatcher 的规则相同，见 worker.h）。所以协程帧中的局部对象
/// 在析构时、以及插件在 executor 上启动的后台任务，都**不能访问插件对象本身**（this）；需要的状态用
/// shared_ptr 持有。
///
/// 错误用返回值（Decision::deny）。抛出的异常会被捕获：on_request 抛异常时这个请求返回 500，
/// on_complete 抛异常时记录下来、继续执行其他插件的
/// on_complete。异常的说明会写进日志，不能含有密钥。
class Middleware {
 public:
  Middleware() = default;
  Middleware(const Middleware&) = delete;
  Middleware& operator=(const Middleware&) = delete;
  Middleware(Middleware&&) = delete;
  Middleware& operator=(Middleware&&) = delete;
  virtual ~Middleware() = default;

  /// 请求开始时调用（读完请求、解析出 model 之后，路由之前）。可以等待（访问外部服务），
  /// 客户端断开或总超时时会被取消。ctx 在整个调用期间有效（协程参数用指针）。
  [[nodiscard]] virtual Task<Decision> on_request(RequestContext* ctx) = 0;

  /// 请求结束时调用，终态在 ctx->outcome。保证：
  /// - 只要这个插件的 on_request 被调用过，on_complete 就一定会被调用一次，不论请求怎样结束
  ///   （成功、被拒绝、上游出错、客户端断开、超时、on_request 抛异常）。on_request 可能没有执行完
  ///   （被取消、抛异常），on_complete 要能处理这种情况
  /// - 不会被客户端断开取消。但整个 on_complete 阶段（所有插件加起来）限时 5 秒，超时的插件被取消、
  ///   **剩下的插件不再执行**（这是保证的第一个例外）、连接被关闭：不能等待没有上限的外部 I/O
  /// - 第二个例外：进程退出时还在进行的请求，协程被直接销毁，on_complete
  /// 不会执行（优雅退出见任务 3.3）
  [[nodiscard]] virtual Task<> on_complete(const RequestContext* ctx) = 0;
};

/// 一个插件在配置中的设置。
struct PluginSpec {
  std::string name;
  /// pipeline 中这一项的 "config" 对象的 JSON 文本；没有写时为 "{}"。插件在自己的 .cpp 中解析
  std::string config_json = "{}";
  /// 在配置文件中的位置（"pipeline[1]"）。validate 报告问题时，路径以它开头
  std::string path;
};

/// Worker 级的资源，传给 MiddlewareFactory::create。
struct WorkerResources {
  unsigned index = 0;
  unsigned worker_count = 1;
  boost::asio::any_io_executor executor;  ///< 这个 Worker 的事件循环
  MetricsRecorder* metrics = nullptr;     ///< 不为空；生命周期长于插件实例
};

/// 插件工厂：每种插件一个，启动时登记到 Registry，之后只读。create() 会被各个 Worker
/// 的线程同时调用， 所以工厂不能有可变状态。
class MiddlewareFactory {
 public:
  MiddlewareFactory() = default;
  MiddlewareFactory(const MiddlewareFactory&) = delete;
  MiddlewareFactory& operator=(const MiddlewareFactory&) = delete;
  MiddlewareFactory(MiddlewareFactory&&) = delete;
  MiddlewareFactory& operator=(MiddlewareFactory&&) = delete;
  virtual ~MiddlewareFactory() = default;

  /// 配置中用的名字：小写字母、数字和下划线
  [[nodiscard]] virtual std::string_view name() const = 0;
  /// 一句话说明（llmgw plugins 中显示）
  [[nodiscard]] virtual std::string_view description() const = 0;
  /// 校验这个插件的配置（以及它依赖的其他配置，比如 keys）。返回的问题与配置文件的其他问题一起报告
  /// （llmgw check、启动时），有错误时配置不生效。只在启动时调用。
  [[nodiscard]] virtual std::vector<config::Issue> validate(
      const PluginSpec& spec, const config::Snapshot& snapshot) const = 0;
  /// 为一个 Worker 创建实例。只在 validate 没有错误之后调用；返回 nullptr 视为启动失败。
  [[nodiscard]] virtual std::unique_ptr<Middleware> create(const PluginSpec& spec,
                                                           const config::Snapshot& snapshot,
                                                           const WorkerResources& worker) const = 0;
};

}  // namespace llmgw::middleware
