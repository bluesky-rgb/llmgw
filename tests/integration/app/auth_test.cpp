// auth 插件的端到端测试（任务 2.2 验证项 A4）：真实的 Server + Gateway + auth 插件 + 测试上游。
// 重点：客户端的网关 key 不会被转发给上游（上游收到的是网关配置的上游 key）。
#include "llmgw/middleware/auth.h"

#include <arpa/inet.h>
#include <gtest/gtest.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <array>
#include <memory>
#include <string>

#include "../upstream/harness.h"
#include "gateway_harness.h"
#include "plugins.h"

namespace llmgw::test {
namespace {

struct AuthGateway {
  explicit AuthGateway(std::string auth_config = "{}") : auth_config_(std::move(auth_config)) {}
  std::string auth_config_;
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
    s->server.workers = 1;  // 鉴权失败的计数每个 Worker 一份：测试限速时要确定
    config::Provider p = provider("http", "127.0.0.1", up.port());
    p.name = "a";
    p.api_key = "sk-UPSTREAM-SECRET";
    s->providers.push_back(p);
    config::TargetConfig t;
    t.provider = "a";
    t.model = "a-model";
    s->models.push_back({.name = "smart", .targets = {t}});
    s->pipeline.push_back({.name = "auth", .config_json = auth_config_, .path = "pipeline[0]"});
    s->keys.push_back({.id = "app", .hash = middleware::hash_key(key)});
    return s;
  }
};

constexpr const char* kBody = R"({"model":"smart","messages":[]})";

// 从指定的本机源地址（127.0.0.x，Linux 上整个 127/8 都可以用）发一个请求，返回完整的响应文本。
// 用原始的 socket：测试的是“网关看到的客户端地址”，需要控制源地址
std::string raw_request_from(const char* source, std::uint16_t port, const std::string& auth) {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return {};
  timeval tv{.tv_sec = 5, .tv_usec = 0};
  ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  sockaddr_in local{};
  local.sin_family = AF_INET;
  ::inet_pton(AF_INET, source, &local.sin_addr);
  sockaddr_in remote{};
  remote.sin_family = AF_INET;
  remote.sin_port = htons(port);
  ::inet_pton(AF_INET, "127.0.0.1", &remote.sin_addr);
  std::string out;
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)：socket API 要求
  if (::bind(fd, reinterpret_cast<sockaddr*>(&local), sizeof(local)) == 0 &&
      // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
      ::connect(fd, reinterpret_cast<sockaddr*>(&remote), sizeof(remote)) == 0) {
    const std::string body = kBody;
    const std::string req =
        "POST /v1/chat/completions HTTP/1.1\r\nHost: t\r\nConnection: close\r\n"
        "Content-Type: application/json\r\n" +
        (auth.empty() ? std::string() : "Authorization: " + auth + "\r\n") +
        "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
    (void)::send(fd, req.data(), req.size(), MSG_NOSIGNAL);
    std::array<char, 4096> buf{};
    for (;;) {
      const ssize_t n = ::recv(fd, buf.data(), buf.size(), 0);
      if (n <= 0) break;
      out.append(buf.data(), static_cast<std::size_t>(n));
    }
  }
  ::close(fd);
  return out;
}

int status_of(const std::string& response) {
  return response.size() > 12 ? std::stoi(response.substr(9, 3)) : 0;
}

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

// 限速按客户端地址：Session 要把真实的对端地址交给插件（地址为空时所有客户端会共用一个计数，
// 单元测试发现不了，独立审查 2.2 #3）。workers = 1：每个 Worker 各自计数
TEST(AuthE2E, failure_throttling_is_per_client_address) {
  AuthGateway g(R"({"failures_per_minute": 2})");
  const std::uint16_t port = g.gw.server->data_port();
  EXPECT_EQ(status_of(raw_request_from("127.0.0.2", port, "Bearer bad")), 401);
  EXPECT_EQ(status_of(raw_request_from("127.0.0.2", port, "Bearer bad")), 401);
  const std::string throttled = raw_request_from("127.0.0.2", port, "Bearer bad");
  EXPECT_EQ(status_of(throttled), 429) << throttled;
  EXPECT_NE(throttled.find("Retry-After: "), std::string::npos) << throttled;
  EXPECT_NE(throttled.find("rate_limited"), std::string::npos);
  EXPECT_EQ(status_of(raw_request_from("127.0.0.3", port, "Bearer bad")), 401)
      << "别的客户端不受影响";
  EXPECT_EQ(status_of(raw_request_from("127.0.0.2", port, "Bearer " + g.key)), 200)
      << "被限速的地址上，有效的 key 照常放行";
  // 多个 Authorization 头：不知道该信哪一个（与前面的代理可能取的不一样），当作没有 key
  EXPECT_EQ(status_of(raw_request_from("127.0.0.4", port,
                                       "Bearer " + g.key + "\r\nAuthorization: Bearer x")),
            401);
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
