// 中间件链（任务 2.1）。
//
// 异常：插件的异常在这里捕获，不传给 Session（Session 的竞速分支不能抛异常，见 relay.cpp）。
// catch 块中不能 co_await：先记下来，块外再检查是否被取消。
#include "llmgw/middleware/chain.h"

#include <cstdio>
#include <exception>
#include <optional>
#include <utility>

namespace llmgw::middleware {

Chain::Chain(std::vector<std::string> names, std::vector<std::unique_ptr<Middleware>> plugins,
             MetricsRecorder* metrics)
    : names_(std::move(names)), plugins_(std::move(plugins)), metrics_(metrics) {
  if (names_.size() != plugins_.size()) names_.resize(plugins_.size(), "unknown");
  if (metrics_ == nullptr) {
    null_metrics_ = std::make_unique<NullMetricsRecorder>();
    metrics_ = null_metrics_.get();
  }
}

Result<std::unique_ptr<Chain>> Chain::build(const config::Snapshot& snapshot,
                                            const Registry& registry,
                                            const WorkerResources& worker) {
  std::vector<std::string> names;
  std::vector<std::unique_ptr<Middleware>> plugins;
  for (const config::PipelineEntry& entry : snapshot.pipeline) {
    const MiddlewareFactory* factory = registry.find(entry.name);
    if (factory == nullptr) {
      return fail(ErrorCode::kInternal, entry.path + "：没有名为 \"" + entry.name + "\" 的插件");
    }
    std::unique_ptr<Middleware> plugin;
    std::string why;
    try {
      plugin = factory->create(
          PluginSpec{.name = entry.name, .config_json = entry.config_json, .path = entry.path},
          snapshot, worker);
    } catch (const std::exception& e) {  // 启动阶段：转换成错误，而不是让异常逃出 Server::start
      why = std::string("：") + e.what();
    }
    if (plugin == nullptr) {
      return fail(ErrorCode::kInternal,
                  entry.path + "：插件 \"" + entry.name + "\" 创建失败" + why);
    }
    names.push_back(entry.name);
    plugins.push_back(std::move(plugin));
  }
  return std::make_unique<Chain>(std::move(names), std::move(plugins), worker.metrics);
}

Task<Decision> Chain::on_request(RequestContext* ctx, Run* run) {
  for (std::size_t i = run->entered; i < plugins_.size(); ++i) {
    // 已经被取消：不再进入下一个插件（co_await 一个协程会直接抛出，插件的代码根本不会执行）
    if (is_cancelled(co_await kCancellationState)) {
      co_return Decision::deny(
          Error{.code = ErrorCode::kInternal, .message = "请求已取消", .retry_after = {}});
    }
    run->entered = i + 1;  // 先记下：on_request 抛异常或被取消时，这个插件也要收到 on_complete
    Decision decision;
    bool threw = false;
    std::string what;
    try {
      decision = co_await plugins_[i]->on_request(ctx);
    } catch (const std::exception& e) {
      threw = true;
      what = e.what();
    } catch (...) {
      threw = true;
      what = "未知异常";
    }
    if (threw) {
      if (!is_cancelled(co_await kCancellationState)) {  // 被取消引起的异常不是插件的错误
        std::fprintf(stderr, "[middleware] request %s: plugin %s on_request threw: %s\n",
                     ctx->request.request_id.c_str(), names_[i].c_str(), what.c_str());
        metrics_->increment(Counter::kMiddlewareErrors, {names_[i], "on_request"});
      }
      run->failed = true;
      co_return Decision::deny(
          Error{.code = ErrorCode::kInternal, .message = "网关内部错误", .retry_after = {}});
    }
    std::ranges::move(decision.response_headers, std::back_inserter(run->response_headers));
    if (decision.reject) co_return Decision::deny(std::move(*decision.reject));
  }
  co_return Decision::pass();
}

Task<> Chain::on_complete(const RequestContext* ctx, Run* run) {
  std::optional<std::size_t> last;  // 最近一个开始执行 on_complete 的插件
  for (std::size_t i = run->entered - run->completed; i-- > 0;) {
    // 被取消（Session 的限时到了）：不再开始下一个。取消发生在上一个插件执行期间，算它超时
    if (is_cancelled(co_await kCancellationState)) {
      run->cut_off = last;
      co_return;
    }
    last = i;
    bool threw = false;
    std::string what;
    try {
      co_await plugins_[i]->on_complete(ctx);
    } catch (const std::exception& e) {
      threw = true;
      what = e.what();
    } catch (...) {
      threw = true;
      what = "未知异常";
    }
    if (!threw) {
      ++run->completed;
      continue;
    }
    // 被取消（Session 的限时到了）：由 Session 记录超时，这里停下
    if (is_cancelled(co_await kCancellationState)) {
      run->cut_off = i;
      co_return;
    }
    std::fprintf(stderr, "[middleware] request %s: plugin %s on_complete threw: %s\n",
                 ctx->request.request_id.c_str(), names_[i].c_str(), what.c_str());
    metrics_->increment(Counter::kMiddlewareErrors, {names_[i], "on_complete"});
    ++run->completed;
  }
  // 最后一个插件超时了但照样返回
  if (is_cancelled(co_await kCancellationState)) run->cut_off = last;
}

}  // namespace llmgw::middleware
