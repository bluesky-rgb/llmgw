// 测试只依赖网关本身用到的库：用 httplib 在进程内起一个"假上游"，
// 再起一个真实的网关，用 HTTP 客户端从外面打请求。
#include <atomic>
#include <functional>
#include <mutex>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include <httplib.h>
#include <nlohmann/json.hpp>

#include "config.h"
#include "gateway.h"
#include "logger.h"
#include "sse.h"
#include "url.h"

using nlohmann::json;

// ---- 极简测试框架 ------------------------------------------------------------
namespace {
int g_failures = 0;
std::vector<std::pair<std::string, std::function<void()>>>& registry() {
  static std::vector<std::pair<std::string, std::function<void()>>> r;
  return r;
}
struct Registrar {
  Registrar(const char* name, std::function<void()> fn) { registry().emplace_back(name, fn); }
};
}  // namespace

#define TEST(name)                                  \
  static void name();                               \
  static Registrar registrar_##name(#name, name);   \
  static void name()

#define CHECK(cond)                                                                     \
  do {                                                                                  \
    if (!(cond)) {                                                                      \
      ++g_failures;                                                                     \
      std::cerr << "  FAILED " << __FILE__ << ":" << __LINE__ << ": " #cond << "\n";     \
    }                                                                                   \
  } while (0)

#define CHECK_EQ(a, b)                                                                  \
  do {                                                                                  \
    auto va_ = (a);                                                                     \
    auto vb_ = (b);                                                                     \
    if (!(va_ == vb_)) {                                                                \
      ++g_failures;                                                                     \
      std::cerr << "  FAILED " << __FILE__ << ":" << __LINE__ << ": " #a " == " #b       \
                << "  (" << va_ << " vs " << vb_ << ")\n";                              \
    }                                                                                   \
  } while (0)

// ---- 假上游 ------------------------------------------------------------------
// /good/v1  正常返回（流式和非流式都支持）
// /bad/v1   总是返回 500
// /busy/v1  总是返回 429
// 请求的 model 为 "invalid" 时返回 400
class MockUpstream {
 public:
  MockUpstream() {
    server_.Post("/good/v1/chat/completions", [this](const httplib::Request& req,
                                                     httplib::Response& res) {
      ++good_hits;
      auto body = json::parse(req.body);
      const std::string model = body.value("model", "");
      {
        std::lock_guard<std::mutex> lock(mu_);  // 并发测试中多个请求同时写这些字段
        last_auth = req.get_header_value("Authorization");
        last_request_id = req.get_header_value("X-Request-Id");
        last_model = model;
      }
      if (model == "invalid") {
        res.status = 400;
        res.set_content(R"({"error":{"message":"bad model","type":"invalid_request_error"}})",
                        "application/json");
        return;
      }
      if (body.value("stream", false)) {
        res.set_chunked_content_provider(
            "text/event-stream", [](size_t, httplib::DataSink& sink) {
              const std::vector<std::string> events = {
                  R"(data: {"choices":[{"delta":{"content":"Hel"}}]})" "\n\n",
                  R"(data: {"choices":[{"delta":{"content":"lo"}}]})" "\n\n",
                  R"(data: {"choices":[],"usage":{"prompt_tokens":7,"completion_tokens":2}})" "\n\n",
                  "data: [DONE]\n\n"};
              for (const auto& e : events) {
                // 故意拆成两半发送，检验网关对任意切块的处理
                const auto half = e.size() / 2;
                sink.write(e.data(), half);
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                sink.write(e.data() + half, e.size() - half);
              }
              sink.done();
              return true;
            });
        return;
      }
      res.set_content(json{{"id", "chatcmpl-1"},
                           {"object", "chat.completion"},
                           {"model", model},
                           {"choices", {{{"index", 0},
                                         {"message", {{"role", "assistant"}, {"content", "Hello"}}},
                                         {"finish_reason", "stop"}}}},
                           {"usage", {{"prompt_tokens", 5}, {"completion_tokens", 1}}}}
                          .dump(),
                      "application/json");
    });
    server_.Post("/bad/v1/chat/completions", [this](const httplib::Request&, httplib::Response& res) {
      ++bad_hits;
      res.status = 500;
      res.set_content(R"({"error":"boom"})", "application/json");
    });
    server_.Post("/busy/v1/chat/completions", [this](const httplib::Request&, httplib::Response& res) {
      ++busy_hits;
      res.status = 429;
      res.set_content(R"({"error":"rate limited"})", "application/json");
    });
    port = server_.bind_to_any_port("127.0.0.1");
    thread_ = std::thread([this] { server_.listen_after_bind(); });
    server_.wait_until_ready();
  }
  ~MockUpstream() {
    server_.stop();
    thread_.join();
  }
  std::string url(const std::string& prefix) const {
    return "http://127.0.0.1:" + std::to_string(port) + prefix;
  }

  int port = 0;
  std::atomic<int> good_hits{0}, bad_hits{0}, busy_hits{0};
  std::string last_auth, last_model, last_request_id;

 private:
  std::mutex mu_;
  httplib::Server server_;
  std::thread thread_;
};

// 测试用的网关：配置一组模型，分别覆盖各种切换场景
class TestGateway {
 public:
  TestGateway(const MockUpstream& up, std::vector<std::string> api_keys = {}) {
    json cfg = {
        {"listen", {{"host", "127.0.0.1"}, {"port", 0}, {"threads", 8}}},
        {"api_keys", api_keys},
        {"providers",
         {{{"name", "good"}, {"base_url", up.url("/good/v1")}, {"api_key", "sk-good"}},
          {{"name", "bad"}, {"base_url", up.url("/bad/v1")}},
          {{"name", "busy"}, {"base_url", up.url("/busy/v1")}},
          // 端口 1 基本不会有服务在监听，用来模拟连接失败
          {{"name", "down"}, {"base_url", "http://127.0.0.1:1/v1"}, {"connect_timeout_seconds", 1}}}},
        {"models",
         {{{"name", "direct"}, {"targets", {{{"provider", "good"}, {"model", "real-model"}}}}},
          {{"name", "failover"},
           {"targets", {{{"provider", "bad"}}, {{"provider", "busy"}}, {{"provider", "good"}, {"model", "backup"}}}}},
          {{"name", "conn-failover"},
           {"targets", {{{"provider", "down"}}, {{"provider", "good"}, {"model", "backup"}}}}},
          {{"name", "all-down"}, {"targets", {{{"provider", "down"}}}}},
          {{"name", "last-bad"}, {"targets", {{{"provider", "bad"}}}}},
          {{"name", "invalid"}, {"targets", {{{"provider", "good"}}}}}}}};
    gw_ = std::make_unique<llmgw::Gateway>(llmgw::parse_config(cfg.dump()));
    port = gw_->bind("127.0.0.1", 0);
    thread_ = std::thread([this] { gw_->run(); });
    gw_->wait_until_ready();
  }
  ~TestGateway() {
    gw_->stop();
    thread_.join();
  }
  httplib::Client client() const {
    httplib::Client c("127.0.0.1", port);
    c.set_read_timeout(10, 0);
    return c;
  }
  llmgw::Metrics& metrics() { return gw_->metrics(); }

  int port = 0;

 private:
  std::unique_ptr<llmgw::Gateway> gw_;
  std::thread thread_;
};

std::string chat_body(const std::string& model, bool stream = false) {
  return json{{"model", model},
              {"stream", stream},
              {"messages", {{{"role", "user"}, {"content", "hi"}}}}}
      .dump();
}

// ---- 单元测试 ----------------------------------------------------------------

TEST(base_url_parsing) {
  auto a = llmgw::parse_base_url("https://api.openai.com/v1/");
  CHECK_EQ(a.scheme_host_port, std::string("https://api.openai.com:443"));
  CHECK_EQ(a.path_prefix, std::string("/v1"));

  auto b = llmgw::parse_base_url("http://127.0.0.1:8081");
  CHECK_EQ(b.scheme_host_port, std::string("http://127.0.0.1:8081"));
  CHECK_EQ(b.path_prefix, std::string(""));

  bool threw = false;
  try {
    llmgw::parse_base_url("ftp://x");
  } catch (const std::invalid_argument&) {
    threw = true;
  }
  CHECK(threw);
}

TEST(config_validation) {
  auto expect_error = [](const std::string& text, const std::string& fragment) {
    try {
      llmgw::parse_config(text);
      std::cerr << "  expected error containing: " << fragment << "\n";
      return false;
    } catch (const std::runtime_error& e) {
      if (std::string(e.what()).find(fragment) == std::string::npos) {
        std::cerr << "  got: " << e.what() << "\n";
        return false;
      }
      return true;
    }
  };
  CHECK(expect_error("{", "JSON"));
  CHECK(expect_error(R"({"providers":[],"models":[]})", "providers"));
  CHECK(expect_error(
      R"({"providers":[{"name":"a","base_url":"http://x"}],
          "models":[{"name":"m","targets":[{"provider":"nope"}]}]})",
      "nope"));
  CHECK(expect_error(
      R"({"providers":[{"name":"a","base_url":"http://x"},{"name":"a","base_url":"http://y"}],
          "models":[{"name":"m","targets":[{"provider":"a"}]}]})",
      "重复"));

  auto cfg = llmgw::parse_config(
      R"({"providers":[{"name":"a","base_url":"http://x/v1"}],
          "models":[{"name":"m","targets":[{"provider":"a"}]}]})");
  CHECK_EQ(cfg.listen.port, 8080);
  CHECK_EQ(cfg.models[0].targets[0].model, std::string("m"));  // 不写 model 时沿用对外名称
}

TEST(sse_usage_scanner_handles_arbitrary_chunking) {
  const std::string stream =
      "data: {\"choices\":[]}\n\n"
      "data: {\"choices\":[],\"usage\":{\"prompt_tokens\":11,\"completion_tokens\":22}}\r\n\r\n"
      "data: [DONE]\n\n";
  // 每次只喂 1 个字节，模拟最糟糕的切块方式
  llmgw::SseUsageScanner s;
  for (char c : stream) s.feed(&c, 1);
  CHECK(s.usage().present);
  CHECK_EQ(s.usage().prompt_tokens, 11);
  CHECK_EQ(s.usage().completion_tokens, 22);
}

// ---- 端到端测试 --------------------------------------------------------------

TEST(health_and_models) {
  MockUpstream up;
  TestGateway gw(up);
  auto c = gw.client();
  auto h = c.Get("/health");
  CHECK(h && h->status == 200);
  auto m = c.Get("/v1/models");
  CHECK(m && m->status == 200);
  auto j = json::parse(m->body);
  CHECK_EQ(j["data"].size(), size_t{6});
}

TEST(non_stream_forwards_and_rewrites_model) {
  MockUpstream up;
  TestGateway gw(up);
  auto c = gw.client();
  auto r = c.Post("/v1/chat/completions", {{"X-Request-Id", "my-req-1"}}, chat_body("direct"),
                  "application/json");
  CHECK(r && r->status == 200);
  CHECK_EQ(up.last_model, std::string("real-model"));
  CHECK_EQ(up.last_auth, std::string("Bearer sk-good"));
  CHECK_EQ(up.last_request_id, std::string("my-req-1"));
  CHECK_EQ(r->get_header_value("X-Request-Id"), std::string("my-req-1"));
  CHECK_EQ(json::parse(r->body)["choices"][0]["message"]["content"], "Hello");
  CHECK(gw.metrics().render().find("type=\"prompt\"} 5") != std::string::npos);
}

TEST(non_stream_fails_over_on_5xx_and_429) {
  MockUpstream up;
  TestGateway gw(up);
  auto r = gw.client().Post("/v1/chat/completions", chat_body("failover"), "application/json");
  CHECK(r && r->status == 200);
  CHECK_EQ(up.bad_hits.load(), 1);
  CHECK_EQ(up.busy_hits.load(), 1);
  CHECK_EQ(up.good_hits.load(), 1);
  CHECK_EQ(up.last_model, std::string("backup"));
  CHECK_EQ(r->get_header_value("X-LLMGW-Provider"), std::string("good"));
}

TEST(non_stream_fails_over_on_connection_error) {
  MockUpstream up;
  TestGateway gw(up);
  auto r = gw.client().Post("/v1/chat/completions", chat_body("conn-failover"), "application/json");
  CHECK(r && r->status == 200);
  CHECK_EQ(up.good_hits.load(), 1);
}

TEST(non_stream_all_down_returns_502) {
  MockUpstream up;
  TestGateway gw(up);
  auto r = gw.client().Post("/v1/chat/completions", chat_body("all-down"), "application/json");
  CHECK(r && r->status == 502);
  CHECK_EQ(json::parse(r->body)["error"]["code"], "upstream_unavailable");
}

TEST(last_target_error_is_passed_through) {
  MockUpstream up;
  TestGateway gw(up);
  auto r = gw.client().Post("/v1/chat/completions", chat_body("last-bad"), "application/json");
  CHECK(r && r->status == 500);
}

TEST(client_errors_are_not_retried) {
  MockUpstream up;
  TestGateway gw(up);
  auto c = gw.client();
  auto r = c.Post("/v1/chat/completions", chat_body("invalid"), "application/json");
  CHECK(r && r->status == 400);
  CHECK_EQ(up.good_hits.load(), 1);

  auto unknown = c.Post("/v1/chat/completions", chat_body("no-such-model"), "application/json");
  CHECK(unknown && unknown->status == 404);
  auto bad_json = c.Post("/v1/chat/completions", "not json", "application/json");
  CHECK(bad_json && bad_json->status == 400);
}

TEST(api_key_required_when_configured) {
  MockUpstream up;
  TestGateway gw(up, {"sk-gw-test"});
  auto c = gw.client();
  auto denied = c.Post("/v1/chat/completions", chat_body("direct"), "application/json");
  CHECK(denied && denied->status == 401);
  auto ok = c.Post("/v1/chat/completions", {{"Authorization", "Bearer sk-gw-test"}},
                   chat_body("direct"), "application/json");
  CHECK(ok && ok->status == 200);
}

TEST(stream_is_relayed_in_order) {
  MockUpstream up;
  TestGateway gw(up);
  std::string received;
  int chunks = 0;
  auto r = gw.client().Post("/v1/chat/completions", httplib::Headers{}, chat_body("direct", true),
                            "application/json", [&](const char* d, size_t n) {
                              received.append(d, n);
                              ++chunks;
                              return true;
                            });
  CHECK(r && r->status == 200);
  CHECK(r->get_header_value("Content-Type").find("text/event-stream") != std::string::npos);
  CHECK(received.find("Hel") < received.find("lo\""));
  CHECK(received.find("data: [DONE]") != std::string::npos);
  CHECK(chunks > 1);  // 确实是分块到达的，不是攒齐了一次性返回
  // 流式请求结束后（释放回调里）才记录指标，稍等一下
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  const auto m = gw.metrics().render();
  CHECK(m.find("type=\"prompt\"} 7") != std::string::npos);
  CHECK(m.find("type=\"completion\"} 2") != std::string::npos);
}

TEST(stream_fails_over_before_first_byte) {
  MockUpstream up;
  TestGateway gw(up);
  std::string received;
  auto r = gw.client().Post("/v1/chat/completions", httplib::Headers{}, chat_body("failover", true),
                            "application/json", [&](const char* d, size_t n) {
                              received.append(d, n);
                              return true;
                            });
  CHECK(r && r->status == 200);
  CHECK_EQ(up.bad_hits.load(), 1);
  CHECK_EQ(up.busy_hits.load(), 1);
  CHECK(received.find("data: [DONE]") != std::string::npos);
}

TEST(stream_upstream_400_keeps_status) {
  MockUpstream up;
  TestGateway gw(up);
  auto r = gw.client().Post("/v1/chat/completions", chat_body("invalid", true), "application/json");
  CHECK(r && r->status == 400);
  CHECK(r->body.find("bad model") != std::string::npos);
}

TEST(stream_all_down_returns_502) {
  MockUpstream up;
  TestGateway gw(up);
  auto r = gw.client().Post("/v1/chat/completions", chat_body("all-down", true), "application/json");
  CHECK(r && r->status == 502);
}

TEST(concurrent_streams) {
  MockUpstream up;
  TestGateway gw(up);
  std::atomic<int> ok{0};
  std::vector<std::thread> threads;
  for (int i = 0; i < 6; ++i) {
    threads.emplace_back([&] {
      std::string received;
      auto r = gw.client().Post("/v1/chat/completions", httplib::Headers{},
                                chat_body("direct", true), "application/json",
                                [&](const char* d, size_t n) {
                                  received.append(d, n);
                                  return true;
                                });
      if (r && r->status == 200 && received.find("[DONE]") != std::string::npos) ++ok;
    });
  }
  for (auto& t : threads) t.join();
  CHECK_EQ(ok.load(), 6);
}

int main() {
  llmgw::set_logging_enabled(false);
  for (const auto& [name, fn] : registry()) {
    const int before = g_failures;
    fn();
    std::cout << (g_failures == before ? "[ OK ] " : "[FAIL] ") << name << "\n";
  }
  std::cout << registry().size() << " tests, " << g_failures << " failed checks\n";
  return g_failures == 0 ? 0 : 1;
}
