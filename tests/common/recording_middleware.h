// 测试用的中间件插件：把每次调用记录到一个日志里，并可以按需拒绝、抛异常、等待（任务 2.1）。
// 单元测试（tests/unit/middleware）和集成测试（tests/integration/server）共用。
#pragma once

#include <boost/asio/as_tuple.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "llmgw/middleware/middleware.h"

namespace llmgw::testing {

/// 一次调用的记录。
struct Call {
  std::string plugin;
  std::string stage;  ///< "request" / "request_done"（on_request 执行完）/ "complete"
  Outcome outcome;    ///< stage == "complete" 时：on_complete 看到的终态
};

/// 调用日志。Session 在 Worker 线程上调用插件，测试在主线程上读取，所以加锁。
class CallLog {
 public:
  void add(Call c) {
    const std::lock_guard lock(mu_);
    calls_.push_back(std::move(c));
  }
  [[nodiscard]] std::vector<Call> calls() const {
    const std::lock_guard lock(mu_);
    return calls_;
  }
  /// "a.request b.request b.complete a.complete" 这样的摘要（不含 request_done）
  [[nodiscard]] std::string summary() const {
    std::string s;
    for (const Call& c : calls()) {
      if (c.stage == "request_done") continue;
      if (!s.empty()) s += ' ';
      s += c.plugin + '.' + c.stage;
    }
    return s;
  }
  [[nodiscard]] std::size_t count(const std::string& stage) const {
    std::size_t n = 0;
    for (const Call& c : calls()) n += c.stage == stage ? 1 : 0;
    return n;
  }
  [[nodiscard]] std::size_t count(const std::string& plugin, const std::string& stage) const {
    std::size_t n = 0;
    for (const Call& c : calls()) n += c.plugin == plugin && c.stage == stage ? 1 : 0;
    return n;
  }
  /// 最后一次 on_complete 看到的终态
  [[nodiscard]] std::optional<Outcome> last_outcome() const {
    std::optional<Outcome> o;
    for (const Call& c : calls()) {
      if (c.stage == "complete") o = c.outcome;
    }
    return o;
  }
  void clear() {
    const std::lock_guard lock(mu_);
    calls_.clear();
  }

 private:
  mutable std::mutex mu_;
  std::vector<Call> calls_;
};

/// 插件的行为。
struct Behavior {
  std::optional<Error> deny;  ///< on_request 拒绝
  bool throw_on_request = false;
  bool throw_on_complete = false;
  std::chrono::milliseconds request_delay{0};  ///< on_request 先等这么久（用于测试取消）
  std::chrono::milliseconds complete_delay{0};  ///< on_complete 先等这么久（用于测试限时）
  /// on_complete 的等待不理会取消（as_tuple：被取消时照样往下执行、正常返回）。用于测试“插件超时了
  /// 但仍然返回”的情况：不能把超时算到下一个插件头上
  bool complete_ignores_cancel = false;
  std::vector<Header> headers;  ///< on_request 附加的响应头
};

class RecordingMiddleware final : public middleware::Middleware {
 public:
  RecordingMiddleware(std::string name, std::shared_ptr<CallLog> log, Behavior behavior)
      : name_(std::move(name)), log_(std::move(log)), behavior_(std::move(behavior)) {}

  Task<middleware::Decision> on_request(RequestContext* /*ctx*/) override {
    log_->add({.plugin = name_, .stage = "request", .outcome = {}});
    if (behavior_.request_delay.count() > 0) {
      boost::asio::steady_timer t(co_await boost::asio::this_coro::executor);
      t.expires_after(behavior_.request_delay);
      co_await t.async_wait(boost::asio::as_tuple(boost::asio::use_awaitable));
    }
    if (behavior_.throw_on_request) throw std::runtime_error("boom in on_request");
    log_->add({.plugin = name_, .stage = "request_done", .outcome = {}});
    middleware::Decision d;
    d.response_headers = behavior_.headers;
    if (behavior_.deny) d.reject = *behavior_.deny;
    co_return d;
  }

  Task<> on_complete(const RequestContext* ctx) override {
    if (behavior_.complete_delay.count() > 0) {
      boost::asio::steady_timer t(co_await boost::asio::this_coro::executor);
      t.expires_after(behavior_.complete_delay);
      if (behavior_.complete_ignores_cancel) {
        co_await t.async_wait(boost::asio::as_tuple(boost::asio::use_awaitable));
        // 被取消时 async_wait 立即结束：再忙等一会儿，模拟“超时之后才返回”
        const auto until = std::chrono::steady_clock::now() + behavior_.complete_delay;
        while (std::chrono::steady_clock::now() < until) std::this_thread::yield();
      } else {
        co_await t.async_wait(boost::asio::use_awaitable);  // 被取消时抛出，测试“超时被取消”
      }
    }
    log_->add({.plugin = name_, .stage = "complete", .outcome = ctx->outcome});
    if (behavior_.throw_on_complete) throw std::runtime_error("boom in on_complete");
  }

 private:
  std::string name_;
  std::shared_ptr<CallLog> log_;
  Behavior behavior_;
};

/// 工厂：名字固定，create 出 RecordingMiddleware；validate 可以按配置文本报错。
class RecordingFactory final : public middleware::MiddlewareFactory {
 public:
  RecordingFactory(std::string name, std::shared_ptr<CallLog> log, Behavior behavior = {})
      : name_(std::move(name)), log_(std::move(log)), behavior_(std::move(behavior)) {}

  [[nodiscard]] std::string_view name() const override { return name_; }
  [[nodiscard]] std::string_view description() const override { return "测试用：记录调用"; }
  [[nodiscard]] std::vector<config::Issue> validate(
      const middleware::PluginSpec& spec, const config::Snapshot& /*snapshot*/) const override {
    if (spec.config_json.find("\"bad\"") != std::string::npos) {
      return {{.severity = config::Severity::kError,
               .path = spec.path + ".config.bad",
               .message = "不能有 bad"}};
    }
    return {};
  }
  [[nodiscard]] std::unique_ptr<middleware::Middleware> create(
      const middleware::PluginSpec& /*spec*/, const config::Snapshot& /*snapshot*/,
      const middleware::WorkerResources& /*worker*/) const override {
    if (fail_create) return nullptr;
    if (throw_create) throw std::runtime_error("cannot connect to state store");
    return std::make_unique<RecordingMiddleware>(name_, log_, behavior_);
  }

  bool fail_create = false;
  bool throw_create = false;

 private:
  std::string name_;
  std::shared_ptr<CallLog> log_;
  Behavior behavior_;
};

}  // namespace llmgw::testing
