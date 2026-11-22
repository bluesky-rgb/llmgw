// 中间件链接进 Session（任务 2.1 验证项 M3–M5）：每一种终态都执行 on_complete；拒绝与响应头；
// on_complete 卡住时的限时。插件是记录调用的测试插件（tests/common/recording_middleware.h）。
#include <gtest/gtest.h>

#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "../../common/recording_middleware.h"
#include "fixture.h"
#include "llmgw/middleware/chain.h"

namespace llmgw::server::test {
namespace {

using llmgw::testing::Behavior;
using llmgw::testing::CallLog;
using llmgw::testing::RecordingMiddleware;
using std::chrono::milliseconds;

// 等 pred 成立，最多 within
bool eventually(const std::function<bool()>& pred, milliseconds within = milliseconds(5000)) {
  const auto end = Clock::now() + within;
  while (Clock::now() < end) {
    if (pred()) return true;
    std::this_thread::sleep_for(milliseconds(10));
  }
  return pred();
}

Error deny_error(ErrorCode code) {
  return Error{.code = code, .message = "denied by test plugin", .retry_after = {}};
}

// 记录 llmgw_middleware_errors_total 的指标实现（Worker 线程写、测试线程读，所以加锁）
class ErrorMetrics final : public MetricsRecorder {
 public:
  [[nodiscard]] std::vector<std::string> seen() const {
    const std::lock_guard lock(mu_);
    return seen_;
  }

 private:
  void do_increment(Counter id, LabelValues labels, std::uint64_t /*n*/) override {
    if (id != Counter::kMiddlewareErrors) return;
    std::string s;
    for (auto l : labels) s += std::string(l) + "/";
    const std::lock_guard lock(mu_);
    seen_.push_back(s);
  }
  void do_observe(Histogram /*id*/, LabelValues /*labels*/, double /*value*/) override {}
  void do_set(Gauge /*id*/, LabelValues /*labels*/, double /*value*/) override {}
  mutable std::mutex mu_;
  std::vector<std::string> seen_;
};

// 各 Worker 的指标都转发到同一个 ErrorMetrics
class Forward final : public MetricsRecorder {
 public:
  explicit Forward(std::shared_ptr<ErrorMetrics> to) : to_(std::move(to)) {}

 private:
  void do_increment(Counter id, LabelValues labels, std::uint64_t n) override {
    to_->increment(id, labels, n);
  }
  void do_observe(Histogram /*id*/, LabelValues /*labels*/, double /*value*/) override {}
  void do_set(Gauge /*id*/, LabelValues /*labels*/, double /*value*/) override {}
  std::shared_ptr<ErrorMetrics> to_;
};

class MiddlewareTest : public ServerTest {
 protected:
  void SetUp() override {}  // 每个用例按需要的插件行为自己启动

  // “恰好一次”：Server 销毁（所有 Session 都结束）之后，每个插件的 on_request 次数都等于
  // on_complete 次数。在 TearDown 中检查，迟到的重复调用也能发现（独立审查 2.1）
  void TearDown() override {
    server_.reset();
    if (!exactly_once_) return;
    for (const std::string name : {"a", "b", "c"}) {
      EXPECT_EQ(log_->count(name, "request"), log_->count(name, "complete"))
          << name << "：" << log_->summary();
    }
  }

  // 启动 Server：链上依次是 plugins 中的插件（每个 Worker 一份实例，共用一个调用日志）
  void start_with(std::vector<std::pair<std::string, Behavior>> plugins,
                  std::shared_ptr<config::Snapshot> cfg = test_config(),
                  milliseconds complete_timeout = milliseconds(5000)) {
    auto log = log_;
    auto errors = metrics_;
    start(
        std::move(cfg),
        [log, plugins](const WorkerInfo& /*info*/,
                       MetricsRecorder* metrics) -> Result<std::unique_ptr<middleware::Chain>> {
          std::vector<std::string> names;
          std::vector<std::unique_ptr<middleware::Middleware>> instances;
          for (const auto& [name, behavior] : plugins) {
            names.push_back(name);
            instances.push_back(std::make_unique<RecordingMiddleware>(name, log, behavior));
          }
          return std::make_unique<middleware::Chain>(std::move(names), std::move(instances),
                                                     metrics);
        },
        complete_timeout,
        [errors](const WorkerInfo& /*info*/) { return std::make_unique<Forward>(errors); });
  }

  // 等到 n 次 on_complete 之后返回最后一次看到的终态
  Outcome wait_complete(std::size_t n) {
    EXPECT_TRUE(eventually([&] { return log_->count("complete") >= n; }))
        << "on_complete 没有被调用：" << log_->summary();
    return log_->last_outcome().value_or(Outcome{});
  }

  std::shared_ptr<CallLog> log_ = std::make_shared<CallLog>();
  std::shared_ptr<ErrorMetrics> metrics_ = std::make_shared<ErrorMetrics>();
  bool exactly_once_ = true;  ///< on_complete 限时的用例中，被跳过的插件按约定不会收到 on_complete
};

constexpr const char* kTwo = "a.request b.request b.complete a.complete";

// ---------- M3：每一种终态都执行 on_complete ----------

TEST_F(MiddlewareTest, success_stream_and_non_stream) {
  start_with({{"a", {}}, {"b", {}}});
  for (const bool stream : {true, false}) {
    log_->clear();
    Client c(server_->data_port());
    c.post_chat("ok", stream);
    auto r = c.read_response();
    ASSERT_TRUE(r.ok);
    EXPECT_EQ(r.msg.result_int(), 200);
    const Outcome o = wait_complete(2);
    EXPECT_EQ(o.kind, OutcomeKind::kSuccess);
    EXPECT_EQ(o.http_status, 200);
    EXPECT_EQ(log_->summary(), kTwo) << "stream=" << stream;
  }
}

TEST_F(MiddlewareTest, connection_is_reused_after_on_complete) {
  start_with({{"a", {}}});
  Client c(server_->data_port());
  for (int i = 0; i < 3; ++i) {
    c.post_chat("ok");
    auto r = c.read_response();
    ASSERT_TRUE(r.ok) << "第 " << i << " 个请求";
  }
  wait_complete(3);
  EXPECT_EQ(log_->summary(), "a.request a.complete a.request a.complete a.request a.complete");
}

TEST_F(MiddlewareTest, upstream_status_passed_through) {
  start_with({{"a", {}}});
  Client c(server_->data_port());
  c.post_chat("status:429");
  auto r = c.read_response();
  ASSERT_TRUE(r.ok);
  EXPECT_EQ(r.msg.result_int(), 429);
  const Outcome o = wait_complete(1);
  EXPECT_EQ(o.http_status, 429) << "终态中是返回给客户端的状态码";
}

TEST_F(MiddlewareTest, dispatcher_errors_and_exceptions) {
  start_with({{"a", {}}, {"b", {}}});
  struct Case {
    std::string model;
    int status;
    OutcomeKind kind;
  };
  for (const Case& k :
       {Case{"nope", 404, OutcomeKind::kRejected},
        Case{"error:10", 502, OutcomeKind::kFailed},  // kUpstreamUnavailable
        Case{"throw", 500, OutcomeKind::kFailed}, Case{"midfail", 200, OutcomeKind::kFailed},
        Case{"nextthrow", 200, OutcomeKind::kFailed}}) {
    log_->clear();
    Client c(server_->data_port());
    c.post_chat(k.model, true, "Connection: close\r\n");
    (void)c.read_until_close_or(milliseconds(3000));
    const Outcome o = wait_complete(2);
    EXPECT_EQ(o.kind, k.kind) << k.model;
    EXPECT_EQ(o.http_status, k.status) << k.model;
    EXPECT_EQ(log_->summary(), kTwo) << k.model;
  }
}

TEST_F(MiddlewareTest, client_disconnects_during_dispatch_and_mid_stream) {
  start_with({{"a", {}}, {"b", {}}});
  for (const std::string model : {"hang", "slow"}) {
    log_->clear();
    {
      Client c(server_->data_port());
      c.post_chat(model);
      if (model == "slow") {
        http::response_parser<http::string_body> p;
        ASSERT_TRUE(c.read_head(&p));
      } else {
        ASSERT_TRUE(eventually([&] { return log_->count("request_done") >= 2; }));
      }
      c.close();
    }
    const Outcome o = wait_complete(2);
    EXPECT_EQ(o.kind, OutcomeKind::kClientAborted) << model;
    EXPECT_EQ(o.http_status, model == "slow" ? 200 : 0) << model;
    EXPECT_EQ(log_->summary(), kTwo) << model;
  }
}

TEST_F(MiddlewareTest, total_timeout_before_and_after_headers_and_idle_timeout) {
  start_with({{"a", {}}, {"b", {}}});  // total = 2 秒，idle = 1 秒
  struct Case {
    std::string model;
    int status;
  };
  for (const Case& k : {Case{"hang", 504}, Case{"slow", 200}, Case{"stall", 200}}) {
    log_->clear();
    Client c(server_->data_port());
    c.post_chat(k.model, true, "Connection: close\r\n");
    (void)c.read_until_close_or(milliseconds(4000));
    const Outcome o = wait_complete(2);
    EXPECT_EQ(o.kind, OutcomeKind::kTimeout) << k.model;
    EXPECT_EQ(o.http_status, k.status) << k.model;
    EXPECT_EQ(log_->summary(), kTwo) << k.model;
  }
}

TEST_F(MiddlewareTest, client_that_stops_reading) {
  auto cfg = test_config();
  cfg->timeouts.total = std::chrono::seconds(30);  // 让写超时先到（否则是总超时）
  start_with({{"a", {}}}, cfg);
  Client c(server_->data_port());
  c.post_chat("big");  // 12.8 MB，一个字节都不读：write_timeout（1 秒）后断开
  const Outcome o = wait_complete(1);
  EXPECT_EQ(o.kind, OutcomeKind::kClientAborted);
  EXPECT_EQ(o.http_status, 200);
}

// ---------- 中间件自己结束请求 ----------

TEST_F(MiddlewareTest, rejection_stops_the_chain_and_skips_dispatch) {
  Behavior deny;
  deny.deny = deny_error(ErrorCode::kRateLimited);
  deny.headers = {{"x-ratelimit-remaining-requests", "0"}};
  start_with({{"a", {}}, {"b", deny}, {"c", {}}});
  Client c(server_->data_port());
  c.post_chat("ok");
  auto r = c.read_response();
  ASSERT_TRUE(r.ok);
  EXPECT_EQ(r.msg.result_int(), 429);
  EXPECT_EQ(r.msg["x-ratelimit-remaining-requests"], "0") << "拒绝时也带上插件附加的响应头";
  EXPECT_EQ(r.msg["x-should-retry"], "true");
  EXPECT_NE(r.msg.body().find("rate_limited"), std::string::npos) << r.msg.body();
  const Outcome o = wait_complete(2);
  EXPECT_EQ(o.kind, OutcomeKind::kRejected);
  EXPECT_EQ(o.http_status, 429);
  EXPECT_EQ(log_->summary(), "a.request b.request b.complete a.complete") << "c 没有被调用";
  EXPECT_EQ(probe_->dispatches, 0) << "被拒绝的请求不能发往上游";
  // 拒绝之后连接可以继续使用
  c.post_chat("ok");
  EXPECT_TRUE(c.read_response().ok);
}

TEST_F(MiddlewareTest, on_request_throw_is_500_and_everyone_entered_completes) {
  Behavior boom;
  boom.throw_on_request = true;
  start_with({{"a", {}}, {"b", boom}, {"c", {}}});
  Client c(server_->data_port());
  c.post_chat("ok");
  auto r = c.read_response();
  ASSERT_TRUE(r.ok);
  EXPECT_EQ(r.msg.result_int(), 500);
  EXPECT_EQ(r.msg.body().find("boom"), std::string::npos) << "异常的说明不能返回给客户端";
  const Outcome o = wait_complete(2);
  EXPECT_EQ(o.kind, OutcomeKind::kFailed);
  EXPECT_EQ(log_->summary(), kTwo);
  EXPECT_EQ(probe_->dispatches, 0);
}

TEST_F(MiddlewareTest, client_disconnect_or_timeout_while_a_plugin_waits) {
  Behavior slow;
  slow.request_delay = std::chrono::seconds(30);
  start_with({{"a", {}}, {"b", slow}, {"c", {}}});
  {  // 客户端断开
    Client c(server_->data_port());
    c.post_chat("ok");
    ASSERT_TRUE(eventually([&] { return log_->count("request") >= 2; }));
    c.close();
    const Outcome o = wait_complete(2);
    EXPECT_EQ(o.kind, OutcomeKind::kClientAborted);
    EXPECT_EQ(log_->summary(), kTwo) << "被取消的 b 也要收到 on_complete，c 没有被调用";
  }
  log_->clear();
  {  // 总超时（2 秒）
    Client c(server_->data_port());
    c.post_chat("ok");
    auto r = c.read_response();
    ASSERT_TRUE(r.ok);
    EXPECT_EQ(r.msg.result_int(), 504);
    const Outcome o = wait_complete(2);
    EXPECT_EQ(o.kind, OutcomeKind::kTimeout);
    EXPECT_EQ(o.http_status, 504);
    EXPECT_EQ(log_->summary(), kTwo);
  }
  EXPECT_EQ(probe_->dispatches, 0);
}

// ---------- M4：响应头 ----------

TEST_F(MiddlewareTest, plugin_headers_on_success_replace_upstream_and_unsafe_ones_are_dropped) {
  Behavior headers;
  headers.headers = {{"x-ratelimit-remaining", "5"},
                     {"x-plugin", "yes"},
                     {"content-length", "1"},              // 网关决定的头：丢弃
                     {"X-Request-Id", "forged"},           // 网关决定的头：丢弃
                     {"x-evil", "a\r\nSet-Cookie: x=1"}};  // 不合法：丢弃
  start_with({{"a", headers}});
  Client c(server_->data_port());
  c.post_chat("upstream_ratelimit", true, "X-Request-Id: mine\r\n");
  auto r = c.read_response();
  ASSERT_TRUE(r.ok);
  EXPECT_EQ(r.msg.result_int(), 200);
  EXPECT_EQ(r.msg["x-plugin"], "yes");
  EXPECT_EQ(r.msg["x-ratelimit-remaining"], "5") << "以网关的为准";
  EXPECT_EQ(std::distance(r.msg.equal_range("x-ratelimit-remaining").first,
                          r.msg.equal_range("x-ratelimit-remaining").second),
            1);
  EXPECT_EQ(r.msg["X-Request-Id"], "mine");
  EXPECT_EQ(r.msg.count("x-evil"), 0U);
  EXPECT_EQ(r.msg.count("Set-Cookie"), 0U);
  EXPECT_NE(r.msg.body().find("data: 0"), std::string::npos) << "响应体不受影响";
}

TEST_F(MiddlewareTest, http_1_0_request) {
  start_with({{"a", {}}});
  Client c(server_->data_port());
  const std::string body = R"({"model":"ok","stream":true})";
  c.send(
      "POST /v1/chat/completions HTTP/1.0\r\nContent-Type: application/json\r\n"
      "Content-Length: " +
      std::to_string(body.size()) + "\r\n\r\n" + body);
  const std::string got = c.read_until_close_or(milliseconds(3000));
  EXPECT_NE(got.find("data: 2"), std::string::npos) << got;
  const Outcome o = wait_complete(1);
  EXPECT_EQ(o.kind, OutcomeKind::kSuccess);
}

TEST_F(MiddlewareTest, plugin_rejection_with_5xx_is_still_rejected) {
  Behavior down;
  down.deny = deny_error(ErrorCode::kOverloaded);  // 比如限流的后端不可用
  start_with({{"a", down}});
  Client c(server_->data_port());
  c.post_chat("ok");
  auto r = c.read_response();
  ASSERT_TRUE(r.ok);
  EXPECT_EQ(r.msg.result_int(), 503);
  const Outcome o = wait_complete(1);
  EXPECT_EQ(o.kind, OutcomeKind::kRejected) << "插件拒绝的，不论状态码，都是 kRejected";
  EXPECT_EQ(o.http_status, 503);
}

TEST_F(MiddlewareTest, plugin_headers_on_gateway_504) {
  Behavior headers;
  headers.headers = {{"x-plugin", "yes"}};
  start_with({{"a", headers}});
  Client c(server_->data_port());
  c.post_chat("hang");  // 总超时（2 秒）：网关自己的 504
  auto r = c.read_response();
  ASSERT_TRUE(r.ok);
  EXPECT_EQ(r.msg.result_int(), 504);
  EXPECT_EQ(r.msg["x-plugin"], "yes");
  wait_complete(1);
}

TEST_F(MiddlewareTest, same_header_from_two_plugins_is_kept_twice) {
  Behavior one;
  one.headers = {{"link", "<a>"}, {"x-ratelimit-remaining", "5"}};
  Behavior two;
  two.headers = {{"link", "<b>"}};
  start_with({{"a", one}, {"b", two}});
  Client c(server_->data_port());
  c.post_chat("upstream_ratelimit");
  auto r = c.read_response();
  ASSERT_TRUE(r.ok);
  std::vector<std::string> links;
  const auto range = r.msg.equal_range("link");
  for (auto it = range.first; it != range.second; ++it) links.emplace_back(it->value());
  EXPECT_EQ(links, (std::vector<std::string>{"<a>", "<b>"}));
  EXPECT_EQ(r.msg["x-ratelimit-remaining"], "5");
  wait_complete(2);
}

// ---------- on_complete 出问题 ----------

TEST_F(MiddlewareTest, on_complete_throw_does_not_affect_others_or_the_connection) {
  Behavior boom;
  boom.throw_on_complete = true;
  start_with({{"a", {}}, {"b", boom}, {"c", {}}});
  Client c(server_->data_port());
  c.post_chat("ok");
  ASSERT_TRUE(c.read_response().ok);
  wait_complete(3);
  EXPECT_EQ(log_->summary(), "a.request b.request c.request c.complete b.complete a.complete");
  c.post_chat("ok");
  EXPECT_TRUE(c.read_response().ok) << "连接可以继续使用";
}

TEST_F(MiddlewareTest, stuck_on_complete_is_cut_off_and_connection_closed) {
  exactly_once_ = false;  // 按约定：超时的 b 被取消、a 被跳过
  Behavior stuck;
  stuck.complete_delay = std::chrono::hours(1);
  start_with({{"a", {}}, {"b", stuck}}, test_config(), milliseconds(300));
  Client c(server_->data_port());
  c.post_chat("ok");
  ASSERT_TRUE(c.read_response().ok) << "响应在 on_complete 之前已经写完";
  const auto t0 = Clock::now();
  EXPECT_TRUE(c.closed_within(milliseconds(3000))) << "限时到了要关闭连接";
  EXPECT_GE(Clock::now() - t0, milliseconds(200));
  EXPECT_EQ(log_->summary(), "a.request b.request");
  EXPECT_EQ(metrics_->seen(), (std::vector<std::string>{"b/complete_timeout/"}));
}

// 插件不理会取消、超时之后照样返回：怪的是它，不是下一个插件（独立审查 2.1 #1）
TEST_F(MiddlewareTest, overrunning_plugin_that_ignores_cancel_is_blamed) {
  exactly_once_ = false;
  Behavior slow;
  slow.complete_delay = milliseconds(500);
  slow.complete_ignores_cancel = true;
  start_with({{"a", {}}, {"b", slow}}, test_config(), milliseconds(100));
  Client c(server_->data_port());
  c.post_chat("ok");
  ASSERT_TRUE(c.read_response().ok);
  EXPECT_TRUE(c.closed_within(milliseconds(3000))) << "a 被跳过了：关闭连接";
  EXPECT_EQ(log_->summary(), "a.request b.request b.complete");
  EXPECT_EQ(metrics_->seen(), (std::vector<std::string>{"b/complete_timeout/"}));
}

}  // namespace
}  // namespace llmgw::server::test
