#include "gateway.h"

#include <utility>

#include "llmgw/executor/executor.h"
#include "llmgw/router/router.h"

namespace llmgw::app {
namespace {

class Gateway final : public server::Dispatcher {
 public:
  Gateway(const server::WorkerInfo& info, std::shared_ptr<const config::Snapshot> config,
          std::shared_ptr<const upstream::TlsContexts> tls)
      : client_(upstream::ClientOptions{.executor = info.executor,
                                        .config = std::move(config),
                                        .tls = std::move(tls),
                                        .worker_count = info.worker_count,
                                        .metrics = nullptr,  // Prometheus 输出在 2.8 中接入
                                        .resolver = {},
                                        .dns_ttl = std::chrono::seconds(60),
                                        .sweep_interval = std::chrono::seconds(10)}),
        router_(router::make_failover()),
        executor_(executor::Options{.client = &client_, .metrics = nullptr}) {}

  Task<Result<std::unique_ptr<ResponseStream>>> dispatch(RequestContext* ctx) override {
    auto route = router_->route(*ctx);
    if (!route) co_return fail(route.error());
    // route 在这个协程帧中，execute 期间一直有效；指针指向 ctx->config 中的快照
    co_return co_await executor_.execute(ctx, route->targets);
  }

 private:
  upstream::Client client_;  // 先于 executor_ 构造、后于它销毁（executor_ 持有它的指针）
  std::unique_ptr<router::RoutingStrategy> router_;
  executor::Executor executor_;
};

}  // namespace

server::DispatcherFactory make_dispatcher_factory(
    std::shared_ptr<const config::Snapshot> config,
    std::shared_ptr<const upstream::TlsContexts> tls) {
  return [config = std::move(config), tls = std::move(tls)](
             const server::WorkerInfo& info) -> std::unique_ptr<server::Dispatcher> {
    return std::make_unique<Gateway>(info, config, tls);
  };
}

}  // namespace llmgw::app
