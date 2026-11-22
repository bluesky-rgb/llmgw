// 任务 1.9 N1：原型（prototype/tests/test_main.cpp）的 13 个端到端测试在新架构上的对应版本。
// 照搬的测试与原型的断言相同；行为有意改变、功能推迟的测试见
// docs/tasks/1.9-new-architecture-default.md 第 2 节。
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <nlohmann/json.hpp>
#include <sstream>

#include "gateway_harness.h"

namespace llmgw::test {
namespace {

using Json = nlohmann::json;

std::uint16_t closed_port() {
  asio::io_context ioc;
  asio::ip::tcp::acceptor a(ioc, asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
  return a.local_endpoint().port();
}

// 一个完整的 JSON 响应（Content-Length 按内容计算：手写的长度写错时网关会一直等剩下的字节）
Reply json_reply(const std::string& status_line, const std::string& body) {
  return Reply{.parts = {Part{{},
                              "HTTP/1.1 " + status_line +
                                  "\r\nContent-Type: application/json\r\nContent-Length: " +
                                  std::to_string(body.size()) + "\r\n\r\n" + body}},
               .close_after = false,
               .rst = false,
               .hang = false};
}

// 原型的假上游：good 正常返回（流式时每个事件拆成两半、间隔 5 ms 发送），model 为 "invalid" 时
// 400； bad 总是 500；busy 总是 429
struct PrototypeUpstreams {
  TestUpstream good{[](const Received& r) {
    const Json body = Json::parse(r.body);
    if (body.value("model", "") == "invalid") {
      return json_reply("400 Bad Request",
                        R"({"error":{"message":"bad model","type":"invalid_request_error"}})");
    }
    if (body.value("stream", false)) {
      Reply rep;
      rep.parts.push_back(Part{{},
                               "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n"
                               "Transfer-Encoding: chunked\r\n\r\n"});
      for (const std::string& e : std::vector<std::string>{
               R"(data: {"choices":[{"delta":{"content":"Hel"}}]})"
               "\n\n",
               R"(data: {"choices":[{"delta":{"content":"lo"}}]})"
               "\n\n",
               R"(data: {"choices":[],"usage":{"prompt_tokens":7,"completion_tokens":2}})"
               "\n\n",
               std::string("data: [DONE]\n\n")}) {
        const std::size_t half = e.size() / 2;  // 故意拆成两半，检验网关对任意切块的处理
        for (const std::string& piece : {e.substr(0, half), e.substr(half)}) {
          std::ostringstream chunk;
          chunk << std::hex << piece.size() << "\r\n" << piece << "\r\n";
          rep.parts.push_back(Part{std::chrono::milliseconds(5), chunk.str()});
        }
      }
      rep.parts.push_back(Part{{}, "0\r\n\r\n"});
      return rep;
    }
    const std::string out = Json{{"id", "chatcmpl-1"},
                                 {"object", "chat.completion"},
                                 {"model", body.value("model", "")},
                                 {"choices",
                                  {{{"index", 0},
                                    {"message", {{"role", "assistant"}, {"content", "Hello"}}},
                                    {"finish_reason", "stop"}}}},
                                 {"usage", {{"prompt_tokens", 5}, {"completion_tokens", 1}}}}
                                .dump();
    return ok(out, "Content-Type: application/json\r\n");
  }};
  TestUpstream bad{[](const Received&) {
    return json_reply("500 Internal Server Error", R"({"error":"boom"})");
  }};
  TestUpstream busy{[](const Received&) {
    return json_reply("429 Too Many Requests", R"({"error":"rate limited"})");
  }};
};

// 原型测试中的配置：同样的 6 个模型
std::shared_ptr<config::Snapshot> prototype_config(const PrototypeUpstreams& up,
                                                   bool retry_on_ambiguous) {
  auto cfg = std::make_shared<config::Snapshot>();
  cfg->server.host = "127.0.0.1";
  cfg->server.port = 0;
  cfg->server.admin = {.host = "127.0.0.1", .port = 0};
  cfg->server.observability = {.host = "127.0.0.1", .port = 0};
  cfg->server.workers = 2;
  cfg->retry.retry_on_ambiguous = retry_on_ambiguous;
  auto add = [&](const char* name, std::uint16_t port, const char* key) {
    config::Provider p = provider("http", "127.0.0.1", port);
    p.name = name;
    p.api_key = key;
    cfg->providers.push_back(p);
  };
  add("good", up.good.port(), "sk-good");
  add("bad", up.bad.port(), "sk-bad");
  add("busy", up.busy.port(), "sk-busy");
  add("down", closed_port(), "sk-down");
  auto model = [&](const char* name, std::vector<std::pair<const char*, const char*>> targets) {
    config::ModelConfig m{.name = name, .targets = {}};
    for (const auto& [provider, upstream_model] : targets) {
      config::TargetConfig t;
      t.provider = provider;
      t.model = upstream_model;
      m.targets.push_back(t);
    }
    cfg->models.push_back(m);
  };
  model("direct", {{"good", "real-model"}});
  model("failover", {{"bad", "failover"}, {"busy", "failover"}, {"good", "backup"}});
  model("conn-failover", {{"down", "conn-failover"}, {"good", "backup"}});
  model("all-down", {{"down", "all-down"}});
  model("last-bad", {{"bad", "last-bad"}});
  model("invalid", {{"good", "invalid"}});
  return cfg;
}

std::string chat_body(const std::string& model, bool stream = false) {
  return Json{
      {"model", model}, {"stream", stream}, {"messages", {{{"role", "user"}, {"content", "hi"}}}}}
      .dump();
}

struct Parity {
  PrototypeUpstreams up;
  Gateway gw;
  Caller c;
  explicit Parity(bool retry_on_ambiguous = false)
      : gw(prototype_config(up, retry_on_ambiguous)), c(gw.server->data_port()) {}
  Fetched get(std::uint16_t port, const std::string& target) {
    Caller other(port);
    upstream::Request req;
    req.method = "GET";
    req.target = target;
    return other.env.get(other.gw, std::move(req), deadlines(2s, 10s));
  }
};

TEST(PrototypeParity, health_and_models) {
  Parity p;
  EXPECT_EQ(p.get(p.gw.server->observability_port(), "/health").status, 200)
      << "/health 在观测面端口";
  const Fetched m = p.get(p.gw.server->data_port(), "/v1/models");
  ASSERT_EQ(m.status, 200);
  EXPECT_EQ(Json::parse(m.body)["data"].size(), 6U);
}

TEST(PrototypeParity, non_stream_forwards_and_rewrites_model) {
  Parity p;
  const Fetched r = p.c.chat(chat_body("direct"), {{"X-Request-Id", "my-req-1"}});
  ASSERT_EQ(r.status, 200) << r.body;
  const auto got = p.up.good.received();
  ASSERT_EQ(got.size(), 1U);
  EXPECT_EQ(Json::parse(got[0].body)["model"], "real-model");
  EXPECT_EQ(got[0].header("Authorization"), "Bearer sk-good");
  EXPECT_EQ(header(r, "X-Request-Id"), "my-req-1");
  EXPECT_EQ(Json::parse(r.body)["choices"][0]["message"]["content"], "Hello");
  // 不同于原型：客户端的 X-Request-Id 不转发给上游（1.9 第 2 节）；用量指标在 2.8
  EXPECT_EQ(got[0].header("X-Request-Id"), "");
}

// 原型对 500 也切换。新架构（design.md 6.1）：500 可能已经被上游处理，默认原样返回
TEST(PrototypeParity, non_stream_fails_over_on_5xx_and_429) {
  {
    Parity p;
    const Fetched r = p.c.chat(chat_body("failover"));
    EXPECT_EQ(r.status, 500) << "默认：500 原样返回，不重试";
    EXPECT_EQ(header(r, "x-should-retry"), "false") << "上游可能已经扣费：告诉 SDK 也不要自动重试";
    EXPECT_EQ(p.up.bad.requests(), 1);
    EXPECT_EQ(p.up.busy.requests(), 0);
    EXPECT_EQ(p.up.good.requests(), 0);
  }
  {
    Parity p(/*retry_on_ambiguous=*/true);  // 与原型相同的行为
    const Fetched r = p.c.chat(chat_body("failover"));
    ASSERT_EQ(r.status, 200);
    EXPECT_EQ(p.up.bad.requests(), 1);
    EXPECT_EQ(p.up.busy.requests(), 1);
    EXPECT_EQ(p.up.good.requests(), 1);
    EXPECT_EQ(Json::parse(p.up.good.received()[0].body)["model"], "backup");
    EXPECT_EQ(header(r, "X-LLMGW-Provider"), "good");
  }
}

TEST(PrototypeParity, non_stream_fails_over_on_connection_error) {
  Parity p;
  const Fetched r = p.c.chat(chat_body("conn-failover"));
  EXPECT_EQ(r.status, 200);
  EXPECT_EQ(p.up.good.requests(), 1);
}

TEST(PrototypeParity, non_stream_all_down_returns_502) {
  Parity p;
  const Fetched r = p.c.chat(chat_body("all-down"));
  ASSERT_EQ(r.status, 502);
  EXPECT_EQ(Json::parse(r.body)["error"]["code"], "upstream_unavailable");
}

TEST(PrototypeParity, last_target_error_is_passed_through) {
  Parity p;
  EXPECT_EQ(p.c.chat(chat_body("last-bad")).status, 500);
}

TEST(PrototypeParity, client_errors_are_not_retried) {
  Parity p;
  EXPECT_EQ(p.c.chat(chat_body("invalid")).status, 400);
  EXPECT_EQ(p.up.good.requests(), 1);
  EXPECT_EQ(p.c.chat(chat_body("no-such-model")).status, 404);
  EXPECT_EQ(p.c.chat("not json").status, 400);
}

// 原型的 api_key_required_when_configured：客户端鉴权在 2.2。1.9 的保护（监听非本机地址时拒绝启动）
// 由 tests/cli/serve_smoke.py 测试

TEST(PrototypeParity, stream_is_relayed_in_order) {
  Parity p;
  upstream::Request req = post(chat_body("direct", true));
  req.target = "/v1/chat/completions";
  int reads = 0;
  std::string received;
  std::string content_type;
  const int status = run(p.c.env.ioc, [&]() -> Task<int> {
    auto sent = co_await p.c.env.client->send(p.c.gw, std::move(req), deadlines(2s, 10s));
    if (!sent) co_return 0;
    for (const auto& h : sent->headers) {
      if (h.name == "Content-Type" || h.name == "content-type") content_type = h.value;
    }
    for (;;) {
      const std::size_t before = received.size();
      auto more = co_await sent->body->read(&received);
      if (!more || !*more) break;
      if (received.size() > before) ++reads;
    }
    co_return sent->status;
  }());
  ASSERT_EQ(status, 200);
  EXPECT_LT(received.find("Hel"), received.find("lo\""));
  EXPECT_NE(received.find("data: [DONE]"), std::string::npos);
  EXPECT_GT(reads, 1) << "确实是分块到达的，不是攒齐了一次性返回";
  EXPECT_NE(content_type.find("text/event-stream"), std::string::npos) << content_type;
  // 用量（原型检查指标；指标在 2.8）：客户端没有要求 usage，网关替它要了，上游的 usage 块不能转发
  EXPECT_EQ(received.find("prompt_tokens"), std::string::npos);
  EXPECT_EQ(Json::parse(p.up.good.received().at(0).body)["stream_options"]["include_usage"], true);
}

TEST(PrototypeParity, stream_fails_over_before_first_byte) {
  {  // 默认：第一个目标的 500 原样返回，后面的上游收到 0 个请求
    Parity p;
    const Fetched r = p.c.chat(chat_body("failover", true));
    EXPECT_EQ(r.status, 500);
    EXPECT_EQ(p.up.busy.requests(), 0);
    EXPECT_EQ(p.up.good.requests(), 0);
  }
  Parity p(/*retry_on_ambiguous=*/true);  // 与原型相同的行为需要打开这个配置
  const Fetched r = p.c.chat(chat_body("failover", true));
  ASSERT_EQ(r.status, 200);
  EXPECT_EQ(p.up.bad.requests(), 1);
  EXPECT_EQ(p.up.busy.requests(), 1);
  EXPECT_NE(r.body.find("data: [DONE]"), std::string::npos);
}

TEST(PrototypeParity, stream_upstream_400_keeps_status) {
  Parity p;
  const Fetched r = p.c.chat(chat_body("invalid", true));
  EXPECT_EQ(r.status, 400);
  EXPECT_NE(r.body.find("bad model"), std::string::npos);
}

TEST(PrototypeParity, stream_all_down_returns_502) {
  Parity p;
  EXPECT_EQ(p.c.chat(chat_body("all-down", true)).status, 502);
}

TEST(PrototypeParity, concurrent_streams) {
  Parity p;
  constexpr int kStreams = 200;  // 原型 6 条
  int ok_streams = 0;
  int finished = 0;
  for (int i = 0; i < kStreams; ++i) {
    asio::co_spawn(
        p.c.env.ioc,
        [&]() -> Task<void> {
          upstream::Request req = post(chat_body("direct", true));
          req.target = "/v1/chat/completions";
          auto f = co_await fetch(p.c.env.client.get(), p.c.gw, std::move(req), deadlines(5s, 10s));
          if (f.ok() && f.status == 200 && f.body.find("[DONE]") != std::string::npos) ++ok_streams;
          ++finished;
        },
        asio::detached);
  }
  for (int i = 0; i < 500 && finished < kStreams; ++i) pump_for(p.c.env.ioc, 20ms);
  EXPECT_EQ(finished, kStreams);
  EXPECT_EQ(ok_streams, kStreams);
}

}  // namespace
}  // namespace llmgw::test
