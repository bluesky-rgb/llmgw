// 任务 1.7 X18：进程内的 Server + Gateway（router + executor + 每个 Worker 的 upstream::Client）+
// 测试上游。 用 upstream::Client 当作 HTTP
// 客户端访问网关。重点包括停止时的对象生命周期（独立审查 1.7 #1： Gateway 中的 Client 必须在
// io_context 销毁之前销毁），需要在 ASan 下运行。
#include <algorithm>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <cctype>
#include <memory>

#include "../upstream/harness.h"
#include "gateway_harness.h"
#include "llmgw/server/server.h"

namespace llmgw::test {
namespace {

// 网关的配置：两个上游、一个模型
std::shared_ptr<config::Snapshot> gateway_config(std::uint16_t first, std::uint16_t second) {
  auto cfg = std::make_shared<config::Snapshot>();
  cfg->server.host = "127.0.0.1";
  cfg->server.port = 0;
  cfg->server.admin = {.host = "127.0.0.1", .port = 0};
  cfg->server.observability = {.host = "127.0.0.1", .port = 0};
  cfg->server.workers = 2;
  for (const auto& [name, port] : {std::pair{"a", first}, std::pair{"b", second}}) {
    config::Provider p = provider("http", "127.0.0.1", port);
    p.name = name;
    p.api_key = std::string("sk-SECRET-") + name;
    cfg->providers.push_back(p);
  }
  config::ModelConfig m{.name = "smart", .targets = {}};
  for (const char* name : {"a", "b"}) {
    config::TargetConfig t;
    t.provider = name;
    t.model = std::string(name) + "-model";
    m.targets.push_back(t);
  }
  cfg->models.push_back(m);
  return cfg;
}

std::uint16_t closed_port() {
  asio::io_context ioc;
  asio::ip::tcp::acceptor a(ioc, asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
  return a.local_endpoint().port();
}

TEST(GatewayE2E, X18_fail_over_non_stream_and_stream) {
  TestUpstream up([](const Received& r) {
    if (r.body.find("\"stream\":true") != std::string::npos) {
      return chunked(
          {"data: {\"choices\":[{\"delta\":{\"content\":\"he\"}}]}\n\n",
           "data: {\"choices\":[],\"usage\":{\"prompt_tokens\":1,\"completion_tokens\":1}}\n\n",
           "data: [DONE]\n\n"});
    }
    return ok(R"({"choices":[{"message":{"content":"hi"}}]})",
              "Content-Type: application/json\r\n");
  });
  Gateway gw(gateway_config(closed_port(), up.port()));
  Caller c(gw.server->data_port());

  const Fetched plain = c.chat(R"({"model":"smart","messages":[]})");
  ASSERT_TRUE(plain.ok()) << plain.error->detail;
  EXPECT_EQ(plain.status, 200);
  EXPECT_EQ(header(plain, "X-LLMGW-Provider"), "b");
  EXPECT_EQ(header(plain, "X-LLMGW-Upstream-Model"), "b-model");
  EXPECT_EQ(header(plain, "X-LLMGW-Attempts"), "2");
  EXPECT_NE(plain.body.find("hi"), std::string::npos);

  const Fetched sse = c.chat(R"({"model":"smart","stream":true,"messages":[]})");
  ASSERT_TRUE(sse.ok());
  EXPECT_NE(sse.body.find("[DONE]"), std::string::npos);
  EXPECT_EQ(sse.body.find("usage"), std::string::npos)
      << "网关加上的 include_usage：usage 块不转发";

  const Fetched missing = c.chat(R"({"model":"nope","messages":[]})");
  EXPECT_EQ(missing.status, 404);
  EXPECT_EQ(up.requests(), 2);
  // 连接池中留有空闲连接：Server 析构时 Gateway 的 Client 要关闭它们（ASan 检查生命周期）
}

TEST(GatewayE2E, X18_shutdown_with_a_request_in_flight) {
  TestUpstream hang([](const Received&) {
    return Reply{.parts = {}, .close_after = false, .rst = false, .hang = true};
  });
  auto gw = std::make_unique<Gateway>(gateway_config(hang.port(), hang.port()));
  Caller c(gw->server->data_port());
  // 请求发出去之后上游不响应；在 dispatch 挂起时停止并销毁 Server（ASan 检查生命周期）
  upstream::Request req = post(R"({"model":"smart","messages":[]})");
  req.target = "/v1/chat/completions";
  auto pending = [&]() -> Task<void> {
    (void)co_await c.env.client->send(c.gw, std::move(req), deadlines(2s, 2s));
  };
  asio::co_spawn(c.env.ioc, pending(), asio::detached);
  for (int i = 0; i < 50 && hang.requests() == 0; ++i) pump_for(c.env.ioc, 20ms);
  ASSERT_EQ(hang.requests(), 1) << "前提：请求已经到达上游，dispatch 正挂起在等响应头";
  gw.reset();
  pump_for(c.env.ioc, 50ms);
  SUCCEED();
}

}  // namespace
}  // namespace llmgw::test
