// server 的基本行为：正常请求、Keep-Alive、路由、错误响应、控制面、请求
// ID、管线化、HTTP/1.0、异常隔离。
#include <set>

#include "fixture.h"

namespace llmgw::server::test {
namespace {

std::string header(const http::response<http::string_body>& r, std::string_view name) {
  return std::string(r[name]);
}

// S1：正常的流式请求
TEST_F(ServerTest, streams_chunks_in_order_with_request_id) {
  Client c(server_->data_port());
  c.post_chat("ok");
  auto r = c.read_response();
  ASSERT_TRUE(r.ok);
  EXPECT_EQ(r.msg.result_int(), 200);
  EXPECT_EQ(header(r.msg, "content-type"), "text/event-stream");
  EXPECT_EQ(header(r.msg, "x-upstream"), "fake");
  EXPECT_EQ(header(r.msg, "X-Request-Id").size(), 16U);
  EXPECT_EQ(r.msg.body(), "data: 0\n\ndata: 1\n\ndata: 2\n\n");
}

// S2：Keep-Alive
TEST_F(ServerTest, serves_several_requests_on_one_connection) {
  Client c(server_->data_port());
  for (int i = 0; i < 3; ++i) {
    c.post_chat("ok");
    auto r = c.read_response();
    ASSERT_TRUE(r.ok) << "第 " << i + 1 << " 个请求";
    EXPECT_EQ(r.msg.result_int(), 200);
  }
  EXPECT_EQ(probe_->dispatches, 3);
}

// S3：/v1/models
TEST_F(ServerTest, lists_configured_models) {
  Client c(server_->data_port());
  c.send("GET /v1/models HTTP/1.1\r\nHost: t\r\n\r\n");
  auto r = c.read_response();
  ASSERT_TRUE(r.ok);
  EXPECT_EQ(r.msg.result_int(), 200);
  EXPECT_NE(r.msg.body().find(R"("object":"list")"), std::string::npos) << r.msg.body();
  EXPECT_NE(r.msg.body().find(R"("id":"smart")"), std::string::npos);
  EXPECT_NE(r.msg.body().find(R"("id":"fast")"), std::string::npos);
}

struct ErrorCase {
  std::string request;
  int status;
  std::string code;
  bool closes;  ///< 回复后是否关闭连接
};

void expect_error_response(std::uint16_t port, const ErrorCase& ec) {
  SCOPED_TRACE(ec.code);
  Client c(port);
  c.send(ec.request);
  auto r = c.read_response();
  ASSERT_TRUE(r.ok);
  EXPECT_EQ(r.msg.result_int(), ec.status);
  EXPECT_NE(r.msg.body().find(R"("code":")" + ec.code + "\""), std::string::npos) << r.msg.body();
  EXPECT_EQ(header(r.msg, "x-should-retry"), "false");
  EXPECT_FALSE(header(r.msg, "X-Request-Id").empty());
  if (ec.closes) {
    EXPECT_TRUE(c.closed_within(std::chrono::milliseconds(2000)));
  }
}

std::string post(const std::string& body, const std::string& path = "/v1/chat/completions") {
  return "POST " + path + " HTTP/1.1\r\nHost: t\r\nContent-Length: " + std::to_string(body.size()) +
         "\r\n\r\n" + body;
}

// S4：请求错误
TEST_F(ServerTest, rejects_bad_requests_with_openai_style_errors) {
  const auto port = server_->data_port();
  expect_error_response(port,
                        {"GET /v2/nothing HTTP/1.1\r\nHost: t\r\n\r\n", 404, "not_found", false});
  expect_error_response(port, {"GET /v1/chat/completions HTTP/1.1\r\nHost: t\r\n\r\n", 405,
                               "method_not_allowed", false});
  expect_error_response(port, {post("not json"), 400, "invalid_request", false});
  expect_error_response(port, {post("[1,2]"), 400, "invalid_request", false});
  expect_error_response(port, {post(R"({"stream":true})"), 400, "invalid_request", false});
  expect_error_response(port, {post(R"({"model":5})"), 400, "invalid_request", false});
  expect_error_response(port,
                        {post(R"({"model":"ok","stream":"yes"})"), 400, "invalid_request", false});
  expect_error_response(port, {post(R"({"model":"nope"})"), 404, "model_not_found", false});
  expect_error_response(port, {"GARBAGE\r\n\r\n", 400, "invalid_request", true});
  expect_error_response(
      port, {"GET / HTTP/1.1\r\nHost: t\r\nX-Big: " + std::string(70 * 1024, 'a') + "\r\n\r\n", 431,
             "headers_too_large", true});
}

TEST_F(ServerTest, rejects_oversized_body_before_reading_it) {
  Client c(server_->data_port());
  // 只发请求头，声明 10 MB 的请求体（上限 64 KB）：网关不等请求体就应该回复 413
  c.send("POST /v1/chat/completions HTTP/1.1\r\nHost: t\r\nContent-Length: 10485760\r\n\r\n");
  auto r = c.read_response();
  ASSERT_TRUE(r.ok) << "没有在读取请求体之前回复";
  EXPECT_EQ(r.msg.result_int(), 413);
  EXPECT_NE(r.msg.body().find("payload_too_large"), std::string::npos);
  EXPECT_EQ(probe_->dispatches, 0);
}

// S12：dispatch() 返回错误
TEST_F(ServerTest, maps_dispatch_errors_to_status_code_and_retry_hints) {
  struct Want {
    ErrorCode code;
    int status;
    const char* retry;
  };
  for (const Want w :
       {Want{ErrorCode::kRateLimited, 429, "true"},
        Want{ErrorCode::kUpstreamUnavailable, 502, "true"},
        Want{ErrorCode::kOverloaded, 503, "true"}, Want{ErrorCode::kUpstreamTimeout, 504, "false"},
        Want{ErrorCode::kInternal, 500, "false"}, Want{ErrorCode::kBudgetExceeded, 402, "false"}}) {
    SCOPED_TRACE(w.status);
    Client c(server_->data_port());
    c.post_chat("error:" + std::to_string(static_cast<int>(w.code)));
    auto r = c.read_response();
    ASSERT_TRUE(r.ok);
    EXPECT_EQ(r.msg.result_int(), w.status);
    EXPECT_EQ(header(r.msg, "x-should-retry"), w.retry);
    EXPECT_EQ(header(r.msg, "Retry-After"), "2");
    EXPECT_EQ(header(r.msg, "retry-after-ms"), "2000");
    EXPECT_NE(
        r.msg.body().find(std::string(R"("type":")") + std::string(error_type(w.code)) + "\""),
        std::string::npos);
  }
}

// S14：控制面
TEST_F(ServerTest, control_plane_serves_health_and_ready) {
  auto get = [](std::uint16_t port, const std::string& path) {
    Client c(port);
    c.send("GET " + path + " HTTP/1.1\r\nHost: t\r\n\r\n");
    return c.read_response();
  };
  auto health = get(server_->observability_port(), "/health");
  ASSERT_TRUE(health.ok);
  EXPECT_EQ(health.msg.result_int(), 200);
  auto ready = get(server_->observability_port(), "/ready");
  ASSERT_TRUE(ready.ok);
  EXPECT_EQ(ready.msg.result_int(), 200);
  EXPECT_EQ(get(server_->observability_port(), "/metrics").msg.result_int(), 404);
  auto admin = get(server_->admin_port(), "/admin/status");
  ASSERT_TRUE(admin.ok) << "管理面应该在监听";
  EXPECT_EQ(admin.msg.result_int(), 404);
  // 三个端口互不相同
  EXPECT_NE(server_->data_port(), server_->admin_port());
  EXPECT_NE(server_->data_port(), server_->observability_port());
}

// S15：请求 ID
TEST_F(ServerTest, keeps_valid_client_request_id_and_replaces_invalid_ones) {
  Client c(server_->data_port());
  c.post_chat("ok", true, "X-Request-Id: my-trace.42_a\r\n");
  EXPECT_EQ(header(c.read_response().msg, "X-Request-Id"), "my-trace.42_a");
  c.post_chat("ok", true, "X-Request-Id: bad id with spaces\r\n");
  const auto replaced = header(c.read_response().msg, "X-Request-Id");
  EXPECT_EQ(replaced.size(), 16U);
  EXPECT_NE(replaced, "bad id with spaces");
}

// S16：HTTP 管线化——流式输出期间客户端发来下一个请求，不能被当作“客户端断开”。
// 必须等第一个响应开始之后再发第二个请求：一次发出的话，网关读第一个请求时可能已经把第二个也读进
// 缓冲区，socket 不会再变为可读，MSG_PEEK 的逻辑就根本不会被执行到（测试会“碰巧通过”）。
TEST_F(ServerTest, pipelined_requests_are_not_mistaken_for_disconnect) {
  Client c(server_->data_port());
  c.post_chat("slow");
  http::response_parser<http::string_body> p1;
  ASSERT_TRUE(c.read_head(&p1));
  EXPECT_EQ(p1.get().result_int(), 200);
  c.post_chat("ok");  // 第一个响应还在流式输出，第二个请求到了：连接变为可读
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  EXPECT_EQ(probe_->streams_destroyed, 0) << "第一个流被误判为客户端断开而取消了";
  EXPECT_GE(probe_->chunks_served, 3) << "第一个流应该还在继续输出";
}

// S16a：HTTP/1.0（nginx 反向代理默认用 1.0 连接后端）
TEST_F(ServerTest, http10_gets_unchunked_body_terminated_by_close) {
  Client c(server_->data_port());
  const std::string body = R"({"model":"ok","stream":true})";
  c.send("POST /v1/chat/completions HTTP/1.0\r\nContent-Length: " + std::to_string(body.size()) +
         "\r\n\r\n" + body);
  const std::string raw = c.read_until_close();
  EXPECT_EQ(raw.rfind("HTTP/1.0 200", 0), 0U) << raw;
  EXPECT_EQ(raw.find("Transfer-Encoding"), std::string::npos) << "HTTP/1.0 不能使用分块传输";
  EXPECT_NE(raw.find("\r\n\r\ndata: 0\n\ndata: 1\n\ndata: 2\n\n"), std::string::npos) << raw;
}

// S17：多个 Worker
TEST_F(ServerTest, connections_are_spread_across_workers) {
  ASSERT_EQ(server_->workers(), 2U);
  std::set<std::string> seen;
  for (int i = 0; i < 40 && seen.size() < 2; ++i) {
    Client c(server_->data_port());
    c.post_chat("worker");
    const auto res = c.read_response();
    seen.insert(header(res.msg, "x-worker"));
    // 任务 1.7：Dispatcher 知道一共有几个 Worker（每个 Worker 的连接池上限 = 总上限 ÷ Worker 数）
    EXPECT_EQ(header(res.msg, "x-worker-count"), "2");
  }
  EXPECT_EQ(seen.size(), 2U) << "40 个连接都落在了同一个 Worker 上";
}

// S18：异常隔离
TEST_F(ServerTest, exception_in_dispatcher_affects_only_that_request) {
  Client bystander(server_->data_port());
  bystander.post_chat("slow");
  http::response_parser<http::string_body> p;
  ASSERT_TRUE(bystander.read_head(&p));

  Client c(server_->data_port());
  c.post_chat("throw");
  auto r = c.read_response();
  ASSERT_TRUE(r.ok);
  EXPECT_EQ(r.msg.result_int(), 500);
  EXPECT_NE(r.msg.body().find("internal_error"), std::string::npos);
  // 同一条连接还能继续用，旁边的流不受影响
  c.post_chat("ok");
  EXPECT_EQ(c.read_response().msg.result_int(), 200);
  // 客户端读完 ok 的响应时，服务端可能还没销毁那个流（两者在不同线程上，没有先后保证）：先等它结束
  for (int i = 0; i < 200 && probe_->streams_destroyed < 1; ++i)
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  EXPECT_EQ(probe_->streams_destroyed.load(), 1) << "只有 ok 那个流结束了，slow 的流应该还在输出";
}

// S19：端口被占用
TEST(ServerStart, reports_which_port_is_taken) {
  asio::io_context ioc;
  tcp::acceptor taken(ioc, tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
  auto cfg = test_config();
  cfg->server.observability.port = taken.local_endpoint().port();
  Server s(ServerOptions{.config = cfg,
                         .dispatcher_factory =
                             [](const WorkerInfo& w) {
                               return std::make_unique<FakeDispatcher>(w,
                                                                       std::make_shared<Probe>());
                             },
                         .metrics_factory = {},
                         .chain_factory = {},
                         .middleware_complete_timeout = std::chrono::milliseconds(5000)});
  auto r = s.start();
  ASSERT_FALSE(r);
  EXPECT_NE(r.error().message.find("观测面"), std::string::npos) << r.error().message;
  EXPECT_NE(r.error().message.find(std::to_string(taken.local_endpoint().port())),
            std::string::npos);
}

}  // namespace
}  // namespace llmgw::server::test
