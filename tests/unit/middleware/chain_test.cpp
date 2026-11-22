// 插件注册表、pipeline 的校验、中间件链（任务 2.1 验证项 M1）。
#include "llmgw/middleware/chain.h"

#include <gtest/gtest.h>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/experimental/awaitable_operators.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <exception>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "../../common/recording_middleware.h"
#include "llmgw/config/loader.h"
#include "llmgw/core/metrics.h"
#include "llmgw/middleware/registry.h"

namespace llmgw::middleware {
namespace {

namespace asio = boost::asio;
using namespace asio::experimental::awaitable_operators;  // NOLINT(google-build-using-namespace)
using testing::Behavior;
using testing::CallLog;
using testing::RecordingFactory;
using testing::RecordingMiddleware;

// 在一个 io_context 上运行协程直到结束；协程抛出的异常会让测试失败
template <class F>
void run(F make_task) {
  asio::io_context ioc;
  std::exception_ptr error;
  asio::co_spawn(ioc, make_task(), [&](std::exception_ptr e, auto&&...) { error = e; });
  ioc.run();
  if (error) std::rethrow_exception(error);
}

Error error_of(ErrorCode code) { return Error{.code = code, .message = "no", .retry_after = {}}; }

struct Fixture {
  std::shared_ptr<CallLog> log = std::make_shared<CallLog>();
  RequestContext ctx{RequestInfo{.request_id = "req-1",
                                 .model = "m",
                                 .stream = false,
                                 .body = "{}",
                                 .received_at = {},
                                 .deadline = {}},
                     nullptr};

  // names[i] 的行为是 behaviors[i]
  Chain chain(const std::vector<std::pair<std::string, Behavior>>& plugins,
              MetricsRecorder* metrics = nullptr) {
    std::vector<std::string> names;
    std::vector<std::unique_ptr<Middleware>> instances;
    for (const auto& [name, behavior] : plugins) {
      names.push_back(name);
      instances.push_back(std::make_unique<RecordingMiddleware>(name, log, behavior));
    }
    return {std::move(names), std::move(instances), metrics};
  }
};

// 记录 increment 调用的指标实现
class CountingMetrics final : public MetricsRecorder {
 public:
  std::vector<std::string> seen;

 private:
  void do_increment(Counter id, LabelValues labels, std::uint64_t /*n*/) override {
    if (id != Counter::kMiddlewareErrors) return;
    std::string s;
    for (auto l : labels) s += std::string(l) + "/";
    seen.push_back(s);
  }
  void do_observe(Histogram /*id*/, LabelValues /*labels*/, double /*value*/) override {}
  void do_set(Gauge /*id*/, LabelValues /*labels*/, double /*value*/) override {}
};

// ---------- 注册表 ----------

TEST(Registry, rejects_duplicate_and_invalid_names_and_lists_sorted) {
  auto log = std::make_shared<CallLog>();
  Registry r;
  ASSERT_TRUE(r.add(std::make_unique<RecordingFactory>("zeta", log)));
  ASSERT_TRUE(r.add(std::make_unique<RecordingFactory>("alpha", log)));
  EXPECT_FALSE(r.add(std::make_unique<RecordingFactory>("alpha", log))) << "重名";
  EXPECT_FALSE(r.add(std::make_unique<RecordingFactory>("Bad-Name", log)));
  EXPECT_FALSE(r.add(std::make_unique<RecordingFactory>("", log)));
  EXPECT_FALSE(r.add(nullptr));
  const auto all = r.all();
  ASSERT_EQ(all.size(), 2U);
  EXPECT_EQ(all[0]->name(), "alpha");
  EXPECT_EQ(all[1]->name(), "zeta");
  EXPECT_NE(r.find("zeta"), nullptr);
  EXPECT_EQ(r.find("zet"), nullptr);
}

TEST(Registry, builtin_registration_succeeds) {
  Registry r;
  // 2.1 时还没有内置插件；2.2 起每加一个插件，这里都能发现名字不合法、重名等问题
  const auto result = register_builtin(&r);
  ASSERT_TRUE(result) << result.error().message;
  for (const auto* f : r.all()) EXPECT_FALSE(f->description().empty()) << f->name();
}

// ---------- pipeline 的校验 ----------

config::Snapshot snapshot_with(std::vector<config::PipelineEntry> pipeline) {
  config::Snapshot s;
  s.pipeline = std::move(pipeline);
  return s;
}

TEST(ValidatePipeline, unknown_name_suggests_and_lists_available) {
  auto log = std::make_shared<CallLog>();
  Registry r;
  ASSERT_TRUE(r.add(std::make_unique<RecordingFactory>("ratelimit", log)));
  ASSERT_TRUE(r.add(std::make_unique<RecordingFactory>("auth", log)));
  auto issues = validate_pipeline(
      snapshot_with({{.name = "ratelimt", .config_json = "{}", .path = "pipeline[0]"},
                     {.name = "auth", .config_json = "{}", .path = "pipeline[1]"},
                     {.name = "cache", .config_json = "{}", .path = "pipeline[2]"}}),
      r);
  ASSERT_EQ(issues.size(), 2U);
  EXPECT_EQ(issues[0].path, "pipeline[0]");
  EXPECT_NE(issues[0].message.find("是不是 \"ratelimit\""), std::string::npos) << issues[0].message;
  EXPECT_NE(issues[0].message.find("auth、ratelimit"), std::string::npos) << issues[0].message;
  EXPECT_EQ(issues[1].path, "pipeline[2]");
  EXPECT_EQ(issues[1].message.find("是不是"), std::string::npos) << "差得太远时不给建议";
}

TEST(ValidatePipeline, empty_registry_says_so_and_plugin_validate_is_called) {
  auto issues = validate_pipeline(
      snapshot_with({{.name = "auth", .config_json = "{}", .path = "pipeline[0]"}}), Registry{});
  ASSERT_EQ(issues.size(), 1U);
  EXPECT_NE(issues[0].message.find("没有内置的插件"), std::string::npos) << issues[0].message;

  auto log = std::make_shared<CallLog>();
  Registry r;
  ASSERT_TRUE(r.add(std::make_unique<RecordingFactory>("auth", log)));
  auto bad = validate_pipeline(
      snapshot_with({{.name = "auth", .config_json = R"({"bad": 1})", .path = "pipeline[0]"}}), r);
  ASSERT_EQ(bad.size(), 1U);
  EXPECT_EQ(bad[0].path, "pipeline[0].config.bad");
}

TEST(ChainBuild, creates_in_order_and_reports_failed_create) {
  auto log = std::make_shared<CallLog>();
  Registry r;
  ASSERT_TRUE(r.add(std::make_unique<RecordingFactory>("b", log)));
  ASSERT_TRUE(r.add(std::make_unique<RecordingFactory>("a", log)));
  const auto snap = snapshot_with({{.name = "b", .config_json = "{}", .path = "pipeline[0]"},
                                   {.name = "a", .config_json = "{}", .path = "pipeline[1]"}});
  auto chain = Chain::build(snap, r, WorkerResources{});
  ASSERT_TRUE(chain) << chain.error().message;
  EXPECT_EQ((*chain)->size(), 2U);
  RequestContext ctx(RequestInfo{}, nullptr);
  Chain::Run progress;
  run([&]() -> Task<> { co_await (*chain)->on_request(&ctx, &progress); });
  EXPECT_EQ(log->summary(), "b.request a.request") << "按 pipeline 的顺序，不是按名字";

  Registry failing;
  auto f = std::make_unique<RecordingFactory>("x", log);
  f->fail_create = true;
  ASSERT_TRUE(failing.add(std::move(f)));
  auto bad =
      Chain::build(snapshot_with({{.name = "x", .config_json = "{}", .path = "pipeline[0]"}}),
                   failing, WorkerResources{});
  ASSERT_FALSE(bad);
  EXPECT_NE(bad.error().message.find("pipeline[0]"), std::string::npos);

  // create 抛异常：转换成错误，不能逃出 Server::start（独立审查 2.1）
  Registry throwing;
  auto t = std::make_unique<RecordingFactory>("y", log);
  t->throw_create = true;
  ASSERT_TRUE(throwing.add(std::move(t)));
  auto thrown =
      Chain::build(snapshot_with({{.name = "y", .config_json = "{}", .path = "pipeline[0]"}}),
                   throwing, WorkerResources{});
  ASSERT_FALSE(thrown);
  EXPECT_NE(thrown.error().message.find("cannot connect"), std::string::npos)
      << thrown.error().message;
}

// ---------- 链的执行 ----------

TEST(Chain, all_pass_then_complete_in_reverse_and_headers_accumulate) {
  Fixture f;
  Behavior with_header;
  with_header.headers = {{"x-a", "1"}};
  Behavior with_header2;
  with_header2.headers = {{"x-c", "3"}};
  auto chain = f.chain({{"a", with_header}, {"b", {}}, {"c", with_header2}});
  Chain::Run progress;
  Decision d;
  run([&]() -> Task<> { d = co_await chain.on_request(&f.ctx, &progress); });
  EXPECT_FALSE(d.reject);
  EXPECT_EQ(progress.entered, 3U);
  EXPECT_EQ(progress.response_headers, (std::vector<Header>{{"x-a", "1"}, {"x-c", "3"}}));
  f.ctx.outcome = Outcome{.kind = OutcomeKind::kSuccess, .http_status = 200, .error = {}};
  run([&]() -> Task<> { co_await chain.on_complete(&f.ctx, &progress); });
  EXPECT_EQ(f.log->summary(), "a.request b.request c.request c.complete b.complete a.complete");
  EXPECT_EQ(f.log->last_outcome()->kind, OutcomeKind::kSuccess);
}

TEST(Chain, reject_stops_later_plugins_and_only_entered_ones_complete) {
  Fixture f;
  Behavior deny;
  deny.deny = error_of(ErrorCode::kRateLimited);
  deny.headers = {{"x-ratelimit-remaining", "0"}};
  auto chain = f.chain({{"a", {}}, {"b", deny}, {"c", {}}});
  Chain::Run progress;
  Decision d;
  run([&]() -> Task<> { d = co_await chain.on_request(&f.ctx, &progress); });
  ASSERT_TRUE(d.reject);
  EXPECT_EQ(d.reject->code, ErrorCode::kRateLimited);
  EXPECT_EQ(progress.entered, 2U);
  EXPECT_EQ(progress.response_headers.size(), 1U) << "拒绝时附加的响应头也要保留";
  run([&]() -> Task<> { co_await chain.on_complete(&f.ctx, &progress); });
  EXPECT_EQ(f.log->summary(), "a.request b.request b.complete a.complete");
}

TEST(Chain, on_request_throw_is_500_and_thrower_still_completes) {
  Fixture f;
  CountingMetrics metrics;
  Behavior boom;
  boom.throw_on_request = true;
  auto chain = f.chain({{"a", {}}, {"b", boom}, {"c", {}}}, &metrics);
  Chain::Run progress;
  Decision d;
  run([&]() -> Task<> { d = co_await chain.on_request(&f.ctx, &progress); });
  ASSERT_TRUE(d.reject);
  EXPECT_EQ(d.reject->code, ErrorCode::kInternal);
  EXPECT_EQ(d.reject->message.find("boom"), std::string::npos) << "异常的说明不返回给客户端";
  EXPECT_EQ(progress.entered, 2U);
  run([&]() -> Task<> { co_await chain.on_complete(&f.ctx, &progress); });
  EXPECT_EQ(f.log->summary(), "a.request b.request b.complete a.complete");
  EXPECT_EQ(metrics.seen, (std::vector<std::string>{"b/on_request/"}));
}

TEST(Chain, on_complete_throw_does_not_stop_the_others) {
  Fixture f;
  CountingMetrics metrics;
  Behavior boom;
  boom.throw_on_complete = true;
  auto chain = f.chain({{"a", {}}, {"b", boom}, {"c", {}}}, &metrics);
  Chain::Run progress;
  run([&]() -> Task<> {
    co_await chain.on_request(&f.ctx, &progress);
    co_await chain.on_complete(&f.ctx, &progress);
  });
  EXPECT_EQ(f.log->count("complete"), 3U) << f.log->summary();
  EXPECT_EQ(metrics.seen, (std::vector<std::string>{"b/on_complete/"}));
}

TEST(Chain, cancelled_during_on_request_stops_and_entered_ones_complete) {
  Fixture f;
  Behavior slow;
  slow.request_delay = std::chrono::seconds(30);
  auto chain = f.chain({{"a", {}}, {"b", slow}, {"c", {}}});
  Chain::Run progress;
  bool timer_won = false;
  run([&]() -> Task<> {
    asio::steady_timer t(co_await asio::this_coro::executor);
    t.expires_after(std::chrono::milliseconds(20));
    auto r = co_await (chain.on_request(&f.ctx, &progress) || t.async_wait(asio::use_awaitable));
    timer_won = r.index() == 1;
    // 竞速之外、没有被取消的协程中收尾（与 Session 的做法相同）
    co_await chain.on_complete(&f.ctx, &progress);
  });
  EXPECT_TRUE(timer_won);
  EXPECT_EQ(progress.entered, 2U) << "c 没有被调用";
  EXPECT_EQ(f.log->summary(), "a.request b.request b.complete a.complete");
}

// 限时（Session 用竞速实现）到了：记下当时正在执行的插件。插件不理会取消、超时之后照样返回时，
// 怪的是它，不是下一个（独立审查 2.1 #1）
TEST(Chain, cut_off_names_the_plugin_that_overran) {
  Fixture f;
  Behavior slow;
  slow.complete_delay = std::chrono::milliseconds(100);
  slow.complete_ignores_cancel = true;
  auto chain = f.chain({{"a", {}}, {"b", slow}, {"c", {}}});
  Chain::Run progress;
  run([&]() -> Task<> {
    co_await chain.on_request(&f.ctx, &progress);
    asio::steady_timer t(co_await asio::this_coro::executor);
    t.expires_after(std::chrono::milliseconds(20));
    co_await (chain.on_complete(&f.ctx, &progress) || t.async_wait(asio::use_awaitable));
  });
  EXPECT_EQ(f.log->summary(), "a.request b.request c.request c.complete b.complete")
      << "c、b 执行完；a 不再执行";
  ASSERT_TRUE(progress.cut_off);
  EXPECT_EQ(chain.name(*progress.cut_off), "b");
  EXPECT_EQ(progress.completed, 2U);
}

TEST(Chain, no_cut_off_when_everything_finished_in_time) {
  Fixture f;
  auto chain = f.chain({{"a", {}}, {"b", {}}});
  Chain::Run progress;
  run([&]() -> Task<> {
    co_await chain.on_request(&f.ctx, &progress);
    co_await chain.on_complete(&f.ctx, &progress);
  });
  EXPECT_FALSE(progress.cut_off);
  EXPECT_EQ(progress.completed, 2U);
}

TEST(Chain, empty_chain_passes) {
  Fixture f;
  auto chain = f.chain({});
  EXPECT_TRUE(chain.empty());
  Chain::Run progress;
  Decision d;
  run([&]() -> Task<> {
    d = co_await chain.on_request(&f.ctx, &progress);
    co_await chain.on_complete(&f.ctx, &progress);
  });
  EXPECT_FALSE(d.reject);
  EXPECT_EQ(progress.entered, 0U);
}

}  // namespace
}  // namespace llmgw::middleware
