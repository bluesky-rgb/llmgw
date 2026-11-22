// auth 插件的端到端测试（任务 2.2 验证项 A4）：真实的 Server + Gateway + auth 插件 + 测试上游。
// 重点：客户端的网关 key 不会被转发给上游（上游收到的是网关配置的上游 key）。
#include "llmgw/middleware/auth.h"

#include <gtest/gtest.h>

#include <memory>
#include <string>

#include "../upstream/harness.h"
#include "gateway_harness.h"
#include "plugins.h"

namespace llmgw::test {
namespace {

struct AuthGateway {
  std::string key = middleware::generate_key();
  TestUpstream up{[](const Received& /*r*/) {
    return ok(R"({"choices":[{"message":{"content":"hi"}}]})",
              "Content-Type: application/json\r\n");
  }};
  std::shared_ptr<config::Snapshot> cfg = make_config();
  Gateway gw{cfg, app::make_chain_factory(cfg, app::builtin_registry())};
  Caller c{gw.server->data_port()};

  std::shared_ptr<config::Snapshot> make_config() {
    auto s = std::make_shared<config::Snapshot>();
    s->server.host = "127.0.0.1";
    s->server.port = 0;
    s->server.admin = {.host = "127.0.0.1", .port = 0};
    s->server.observability = {.host = "127.0.0.1", .port = 0};
    s->server.workers = 2;
    config::Provider p = provider("http", "127.0.0.1", up.port());
    p.name = "a";
    p.api_key = "sk-UPSTREAM-SECRET";
    s->providers.push_back(p);
    config::TargetConfig t;
    t.provider = "a";
    t.model = "a-model";
    s->models.push_back({.name = "smart", .targets = {t}});
    s->pipeline.push_back({.name = "auth", .config_json = "{}", .path = "pipeline[0]"});
    s->keys.push_back({.id = "app", .hash = middleware::hash_key(key)});
    return s;
  }
};

constexpr const char* kBody = R"({"model":"smart","messages":[]})";

TEST(AuthE2E, requests_without_a_valid_key_never_reach_the_upstream) {
  AuthGateway g;
  for (const std::string& auth :
       {std::string(), std::string("Bearer sk-gw-wrong"), "Basic " + g.key, "Bearer" + g.key}) {
    const Fetched r = g.c.call_with_auth(auth, kBody);
    ASSERT_TRUE(r.ok()) << auth;
    EXPECT_EQ(r.status, 401) << auth;
    EXPECT_EQ(header(r, "WWW-Authenticate"), "Bearer");
    EXPECT_EQ(header(r, "x-should-retry"), "false");
    EXPECT_NE(r.body.find("invalid_api_key"), std::string::npos) << r.body;
    EXPECT_EQ(r.body.find(g.key), std::string::npos);
  }
  EXPECT_EQ(g.up.requests(), 0);
}

TEST(AuthE2E, valid_key_reaches_upstream_with_the_upstream_key_only) {
  AuthGateway g;
  const Fetched r = g.c.call_with_auth("Bearer " + g.key, kBody);
  ASSERT_TRUE(r.ok());
  EXPECT_EQ(r.status, 200) << r.body;
  EXPECT_NE(r.body.find("hi"), std::string::npos);
  // 大小写不敏感的 scheme、多余的空格
  EXPECT_EQ(g.c.call_with_auth("bearer   " + g.key, kBody).status, 200);
  ASSERT_EQ(g.up.requests(), 2);
  for (const Received& got : g.up.received()) {
    EXPECT_EQ(got.header("Authorization"), "Bearer sk-UPSTREAM-SECRET");
    for (const Header& h : got.headers) {
      EXPECT_EQ(h.value.find(g.key), std::string::npos) << "网关 key 被转发给了上游：" << h.name;
    }
    EXPECT_EQ(got.body.find(g.key), std::string::npos);
  }
}

TEST(AuthE2E, models_endpoint_requires_a_key) {
  AuthGateway g;
  const Fetched denied = g.c.call_with_auth("", "", "GET", "/v1/models");
  ASSERT_TRUE(denied.ok());
  EXPECT_EQ(denied.status, 401);
  EXPECT_EQ(denied.body.find("smart"), std::string::npos) << "没有 key 时不能列出模型";
  const Fetched allowed = g.c.call_with_auth("Bearer " + g.key, "", "GET", "/v1/models");
  ASSERT_TRUE(allowed.ok());
  EXPECT_EQ(allowed.status, 200);
  EXPECT_NE(allowed.body.find("smart"), std::string::npos) << allowed.body;
}

}  // namespace
}  // namespace llmgw::test
