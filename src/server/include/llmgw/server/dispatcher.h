// Dispatcher：Session 把“从请求得到响应”这件事交给它。
//
// 1.7 中由 app 组装：dispatch() = router.resolve() + executor.execute()。
// 中间件链（2.1）由 Session 在 dispatch() 之前调用，不放进这里——Session 是唯一的编排者。
// 在 router、executor 实现之前，server 的全部行为都可以用这个接口的测试替身来验证。
#pragma once

#include <memory>

#include "llmgw/core/request_context.h"
#include "llmgw/core/response.h"
#include "llmgw/core/result.h"
#include "llmgw/core/task.h"

namespace llmgw::server {

/// 每个 Worker 一个实例（由 DispatcherFactory 创建），只在该 Worker 的线程上调用，
/// 所以实现可以持有 Worker 级的资源（连接池、熔断器）而不需要加锁。
class Dispatcher {
 public:
  Dispatcher() = default;
  Dispatcher(const Dispatcher&) = delete;
  Dispatcher& operator=(const Dispatcher&) = delete;
  Dispatcher(Dispatcher&&) = delete;
  Dispatcher& operator=(Dispatcher&&) = delete;
  virtual ~Dispatcher() = default;

  /// 处理一个请求。
  /// - 成功：已经拿到响应头的 ResponseStream（包括上游原样透传的 4xx）
  /// - 失败：网关自己的错误（模型不存在、所有上游都失败 …）
  ///
  /// ctx 在整个调用期间由 Session 保证存活（协程参数用指针，code-architecture.md 3.4）。
  /// 客户端断开或总超时时，这个协程会被取消（正在等待的操作以 operation_aborted 结束）。
  [[nodiscard]] virtual Task<Result<std::unique_ptr<ResponseStream>>> dispatch(
      RequestContext* ctx) = 0;
};

}  // namespace llmgw::server
