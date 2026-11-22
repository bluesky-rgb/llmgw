// auth 插件、generate_key、hash_key（任务 2.2 验证项 A1、A2）。
#include "llmgw/middleware/auth.h"

#include <gtest/gtest.h>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <chrono>
#include <exception>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "llmgw/core/metrics.h"

namespace llmgw::middleware {
namespace {

namespace asio = boost::asio;
using std::chrono::seconds;

template <class T>
T run(Task<T> task) {
  asio::io_context ioc;
  std::exception_ptr error;
  T result{};
  asio::co_spawn(ioc, std::move(task), [&](std::exception_ptr e, T value) {
    error = e;
    result = std::move(value);
  });
  ioc.run();
  if (error) std::rethrow_exception(error);
  return result;
}

// ---------- A1：key 与哈希 ----------

TEST(GenerateKey, format_and_uniqueness) {
  std::set<std::string> seen;
  for (int i = 0; i < 1000; ++i) {
    const std::string k = generate_key();
    ASSERT_EQ(k.size(), 6U + 43U) << k;  // "sk-gw-" + 32 字节的 base64url（不补 =）
    ASSERT_TRUE(k.starts_with("sk-gw-"));
    for (const char c : k.substr(6)) {
      ASSERT_TRUE(std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '-' || c == '_') << k;
    }
    seen.insert(k);
  }
  EXPECT_EQ(seen.size(), 1000U);
}

TEST(HashKey, matches_known_sha256_vectors) {
  // FIPS 180-2 的测试向量
  EXPECT_EQ(hash_key("abc"),
            "sha256:ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  EXPECT_EQ(hash_key(""),
            "sha256:e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
}

// ---------- A2：插件 ----------

class CountingMetrics final : public MetricsRecorder {
 public:
  std::map<std::string, int> rejected;

 private:
  void do_increment(Counter id, LabelValues labels, std::uint64_t n) override {
    if (id == Counter::kRejected) rejected[std::string(*labels.begin())] += static_cast<int>(n);
  }
  void do_observe(Histogram /*id*/, LabelValues /*labels*/, double /*value*/) override {}
  void do_set(Gauge /*id*/, LabelValues /*labels*/, double /*value*/) override {}
};

struct AuthFixture {
  std::string key = generate_key();
  config::Snapshot snapshot;
  CountingMetrics metrics;
  std::unique_ptr<MiddlewareFactory> factory = make_auth_factory();
  std::unique_ptr<Middleware> auth;
  SteadyClock::time_point t0 = SteadyClock::now();

  explicit AuthFixture(std::string config_json = "{}") {
    snapshot.keys.push_back({.id = "app", .hash = hash_key(key)});
    snapshot.keys.push_back({.id = "other", .hash = hash_key(generate_key())});
    const PluginSpec spec{
        .name = "auth", .config_json = std::move(config_json), .path = "pipeline[0]"};
    EXPECT_TRUE(factory->validate(spec, snapshot).empty());
    WorkerResources worker;
    worker.metrics = &metrics;
    auth = factory->create(spec, snapshot, worker);
  }

  struct Result {
    Decision decision;
    std::string key_id;
  };
  Result call(const std::string& token, const std::string& address = "203.0.113.7",
              seconds at = seconds(0)) {
    RequestContext ctx(RequestInfo{.request_id = "r",
                                   .model = "m",
                                   .stream = false,
                                   .body = "{}",
                                   .received_at = t0 + at,
                                   .deadline = {},
                                   .bearer_token = token,
                                   .client_address = address},
                       nullptr);
    Decision d = run(auth->on_request(&ctx));
    return {std::move(d), ctx.auth.key_id};
  }
};

int status_of(const Decision& d) { return d.reject ? http_status(d.reject->code) : 200; }

TEST(Auth, valid_key_passes_and_sets_key_id) {
  AuthFixture f;
  auto r = f.call(f.key);
  EXPECT_FALSE(r.decision.reject);
  EXPECT_EQ(r.key_id, "app");
}

TEST(Auth, missing_wrong_or_near_miss_keys_are_401_with_www_authenticate) {
  AuthFixture f;
  for (const std::string& bad : {std::string(), std::string("sk-gw-wrong"), f.key + "x",
                                 f.key.substr(0, f.key.size() - 1), hash_key(f.key)}) {
    auto r = f.call(bad);
    ASSERT_TRUE(r.decision.reject) << bad;
    EXPECT_EQ(r.decision.reject->code, ErrorCode::kInvalidApiKey);
    EXPECT_TRUE(r.key_id.empty());
    EXPECT_EQ(r.decision.reject->message.find(bad.empty() ? "@@" : bad), std::string::npos)
        << "错误信息中不能回显 token";
    ASSERT_EQ(r.decision.response_headers.size(), 1U);
    EXPECT_EQ(r.decision.response_headers[0], (Header{"WWW-Authenticate", "Bearer"}));
  }
  EXPECT_EQ(f.metrics.rejected["auth"], 5);
}

TEST(Auth, failures_are_throttled_per_address_but_valid_keys_never_are) {
  AuthFixture f(R"({"failures_per_minute": 3})");
  for (int i = 0; i < 3; ++i) EXPECT_EQ(status_of(f.call("bad").decision), 401) << i;
  auto throttled = f.call("bad", "203.0.113.7", seconds(10));
  EXPECT_EQ(status_of(throttled.decision), 429);
  ASSERT_TRUE(throttled.decision.reject->retry_after);
  EXPECT_EQ(throttled.decision.reject->retry_after->count(), 50) << "窗口还剩 50 秒";
  EXPECT_EQ(status_of(f.call("", "203.0.113.7", seconds(11)).decision), 429) << "没有 key 也算失败";
  // 同一个地址上的有效 key 不受影响（同一个出口地址后面的正常应用）
  EXPECT_EQ(status_of(f.call(f.key, "203.0.113.7", seconds(12)).decision), 200);
  // 别的地址不受影响
  EXPECT_EQ(status_of(f.call("bad", "198.51.100.1", seconds(12)).decision), 401);
  // 窗口过去之后恢复
  EXPECT_EQ(status_of(f.call("bad", "203.0.113.7", seconds(61)).decision), 401);
  EXPECT_EQ(f.metrics.rejected["auth_throttled"], 2);
}

TEST(Auth, ipv6_is_counted_per_64_prefix) {
  AuthFixture f(R"({"failures_per_minute": 2})");
  EXPECT_EQ(status_of(f.call("bad", "2001:db8:1:2::1").decision), 401);
  EXPECT_EQ(status_of(f.call("bad", "2001:db8:1:2:ffff::9").decision), 401);
  EXPECT_EQ(status_of(f.call("bad", "2001:db8:1:2:abcd::5").decision), 429)
      << "同一个 /64 中换地址不能绕过限速";
  EXPECT_EQ(status_of(f.call("bad", "2001:db8:1:3::1").decision), 401) << "别的 /64";
}

TEST(Auth, full_address_table_stops_tracking_but_still_authenticates) {
  AuthFixture f(R"({"failures_per_minute": 1})");
  for (int i = 0; i < 4096; ++i) {
    (void)f.call("bad", "10.0." + std::to_string(i / 256) + "." + std::to_string(i % 256));
  }
  // 表满了、都没过期：新地址不再被记录（不限速），但鉴权照常
  EXPECT_EQ(status_of(f.call("bad", "192.0.2.1").decision), 401);
  EXPECT_EQ(status_of(f.call("bad", "192.0.2.1").decision), 401) << "没有被记录，所以不会 429";
  EXPECT_EQ(status_of(f.call(f.key, "192.0.2.1").decision), 200);
  // 已经记录的地址照样限速
  EXPECT_EQ(status_of(f.call("bad", "10.0.0.0").decision), 429);
  // 过期之后（每秒最多清理一次）新地址又能被记录
  EXPECT_EQ(status_of(f.call("bad", "192.0.2.2", seconds(61)).decision), 401);
  EXPECT_EQ(status_of(f.call("bad", "192.0.2.2", seconds(61)).decision), 429);
}

TEST(AuthFactory, validates_its_config_and_requires_keys) {
  auto factory = make_auth_factory();
  config::Snapshot with_keys;
  with_keys.keys.push_back({.id = "a", .hash = hash_key("x")});
  auto issues = [&](const std::string& cfg, const config::Snapshot& s) {
    return factory->validate({.name = "auth", .config_json = cfg, .path = "pipeline[0]"}, s);
  };
  EXPECT_TRUE(issues("{}", with_keys).empty());
  EXPECT_TRUE(issues(R"({"failures_per_minute": 100})", with_keys).empty());
  auto bad = issues(R"({"failures_per_minute": 0, "typo": 1})", with_keys);
  ASSERT_EQ(bad.size(), 2U);
  EXPECT_EQ(bad[0].path, "pipeline[0].config.failures_per_minute");
  EXPECT_EQ(bad[1].path, "pipeline[0].config.typo");
  auto no_keys = issues("{}", config::Snapshot{});
  ASSERT_EQ(no_keys.size(), 1U);
  EXPECT_NE(no_keys[0].message.find("keygen"), std::string::npos) << no_keys[0].message;
}

}  // namespace
}  // namespace llmgw::middleware
