// 独立审查发现的问题的回归测试（任务 1.3 之后的排查）。每个测试对应审查报告中的一条。
#include <filesystem>

#include "fixture.h"

namespace llmgw::server::test {
namespace {

using std::chrono::milliseconds;

// 审查 #1（严重）：客户端只发不收，写响应没有超时，可以永久占住连接名额
TEST_F(ServerTest, client_that_never_reads_is_dropped_after_write_timeout) {
  auto cfg = test_config();
  cfg->server.max_connections = 1;
  start(cfg);
  Client hog(server_->data_port());
  std::string many;
  for (int i = 0; i < 2000; ++i) many += "GET /v1/models HTTP/1.1\r\nHost: t\r\n\r\n";
  // 管线化发出大量请求（约 30 万个响应、几十 MB），然后再也不读：服务端的发送缓冲区很快填满
  for (int i = 0; i < 150 && hog.try_send(many); ++i) {
  }
  // 轮询而不是固定等待：本机回环上内核会把收发缓冲区自动调大到几 MB，服务端要先把它们写满
  // （这期间写一直有进展，不算超时）才开始计 write_timeout（1 秒）。sanitizer 构建慢 5–15 倍，
  // 写满缓冲区本身就要好几秒。没有写超时的话名额会被永远占着，所以这里只要求“有限时间内释放”
  const auto t0 = Clock::now();
  bool freed = false;
  while (!freed && Clock::now() - t0 < std::chrono::seconds(15)) {
    Client next(server_->data_port());
    next.post_chat("ok");
    const auto r = next.read_response();
    freed = r.ok && r.msg.result_int() == 200;  // 名额被占时是 503
    if (!freed) std::this_thread::sleep_for(milliseconds(200));
  }
  EXPECT_TRUE(freed) << "不读响应的客户端一直占着唯一的连接名额";
  std::printf(
      "  [名额在停止发送后 %lld ms 释放]\n",
      static_cast<long long>(std::chrono::duration_cast<milliseconds>(Clock::now() - t0).count()));
}

TEST_F(ServerTest, stream_to_client_that_stopped_reading_is_dropped_after_write_timeout) {
  Client c(server_->data_port());
  c.post_chat("big");  // 12.8 MB，客户端一个字节都不读
  for (int i = 0; i < 60 && probe_->streams_destroyed == 0; ++i)
    std::this_thread::sleep_for(milliseconds(50));
  EXPECT_EQ(probe_->streams_destroyed, 1) << "客户端不读时，流应该在 write_timeout（1 秒）后结束";
}

// 审查 #5：客户端读得慢（背压）不能被当作“上游空闲”
TEST_F(ServerTest, slow_reader_is_not_mistaken_for_idle_upstream) {
  auto cfg = test_config();
  cfg->timeouts.total = std::chrono::seconds(30);
  start(cfg);
  Client c(server_->data_port());
  c.post_chat("big");  // 上游立即产出全部数据，之后的时间都花在等客户端读
  http::response_parser<http::string_body> p;
  ASSERT_TRUE(c.read_head(&p));
  // 每 100 ms 读 32 KB（约 320 KB/s）：比上游慢得多，但每一块数据都能在 write_timeout（1
  // 秒）内被读走。 期间上游早已产出完毕，网关一直在“等客户端读”，而不是在“等上游”
  std::size_t got = 0;
  const auto start = Clock::now();
  while (Clock::now() - start < milliseconds(3000)) {
    std::this_thread::sleep_for(milliseconds(100));
    for (int k = 0; k < 2; ++k) {
      const auto chunk = c.read_some(16384);
      ASSERT_FALSE(chunk.empty()) << "读了 " << got << " 字节后连接被断开";
      got += chunk.size();
    }
  }
  EXPECT_EQ(probe_->streams_destroyed, 0) << "客户端一直在读，只是比上游慢：不能按上游空闲超时切断";
  EXPECT_GT(got, 500U * 1024) << "测试应该至少读了 500 KB";
}

// 审查 #2：竞速分支中的异常不能被吞掉，导致客户端挂到总超时
TEST_F(ServerTest, exception_from_next_ends_stream_promptly) {
  Client c(server_->data_port());
  const auto start = Clock::now();
  c.post_chat("nextthrow");
  auto r = c.read_response();
  ASSERT_TRUE(r.ok) << "next() 抛异常后，流式响应应该以错误事件正常结束";
  EXPECT_NE(r.msg.body().find("internal_error"), std::string::npos) << r.msg.body();
  EXPECT_LT(Clock::now() - start, milliseconds(500)) << "不应该等到空闲超时或总超时";
}

TEST_F(ServerTest, invalid_status_from_dispatcher_returns_500_promptly) {
  Client c(server_->data_port());
  const auto start = Clock::now();
  c.post_chat("badstatus");
  auto r = c.read_response();
  ASSERT_TRUE(r.ok);
  EXPECT_EQ(r.msg.result_int(), 500);
  EXPECT_LT(Clock::now() - start, milliseconds(500)) << "不应该挂到总超时再返回 504";
}

// client-guide 第 7 节：流式输出中途出错（包括超时）时，以一条 SSE 错误事件结束
TEST_F(ServerTest, timeout_after_head_ends_stream_with_error_event) {
  Client c(server_->data_port());
  c.post_chat("stall");  // 1 块之后一直不再发送：空闲超时（1 秒）
  auto r = c.read_response();
  ASSERT_TRUE(r.ok) << "流式请求超时后应该以错误事件和结束分块正常结束";
  EXPECT_NE(r.msg.body().find("data: {\"error\":"), std::string::npos) << r.msg.body();
  EXPECT_NE(r.msg.body().find("upstream_timeout"), std::string::npos);
  EXPECT_TRUE(c.closed_within(milliseconds(1000))) << "超时之后连接不能再复用";
}

// 审查 #11：几处 HTTP 细节
TEST_F(ServerTest, malformed_chunked_body_gets_400) {
  Client c(server_->data_port());
  c.send(
      "POST /v1/chat/completions HTTP/1.1\r\nHost: t\r\nTransfer-Encoding: "
      "chunked\r\n\r\nzz\r\nabc\r\n0\r\n\r\n");
  auto r = c.read_response();
  ASSERT_TRUE(r.ok) << "畸形的分块编码应该回复 400，而不是直接断开";
  EXPECT_EQ(r.msg.result_int(), 400);
}

TEST_F(ServerTest, payload_too_large_keeps_client_request_id) {
  Client c(server_->data_port());
  c.send(
      "POST /v1/chat/completions HTTP/1.1\r\nHost: t\r\nX-Request-Id: trace-413\r\nContent-Length: "
      "10485760\r\n\r\n");
  auto r = c.read_response();
  ASSERT_TRUE(r.ok);
  EXPECT_EQ(r.msg.result_int(), 413);
  EXPECT_EQ(r.msg["X-Request-Id"], "trace-413");
}

TEST_F(ServerTest, overloaded_response_has_full_headers) {
  auto cfg = test_config();
  cfg->server.max_connections = 1;
  start(cfg);
  Client hold(server_->data_port());
  hold.post_chat("slow");
  http::response_parser<http::string_body> p;
  ASSERT_TRUE(hold.read_head(&p));
  Client extra(server_->data_port());
  auto r = extra.read_response();
  ASSERT_TRUE(r.ok);
  EXPECT_EQ(r.msg.result_int(), 503);
  EXPECT_EQ(r.msg["X-Request-Id"].size(), 16U);
  EXPECT_EQ(r.msg["retry-after-ms"], "1000");
}

// 审查 #13：S16 声称“两个请求都得到正确的响应”，原来只验证了第一个没被取消
TEST_F(ServerTest, pipelined_request_during_stream_is_answered_after_it) {
  Client c(server_->data_port());
  c.post_chat("medium");  // 5 块 × 50 ms
  http::response_parser<http::string_body> p1;
  ASSERT_TRUE(c.read_head(&p1));
  c.post_chat("ok");  // 第一个响应还在输出时发来
  auto rest = c.read_until_close_or(milliseconds(2500));
  EXPECT_NE(rest.find("data: 4\n\n"), std::string::npos) << "第一个流应该完整输出";
  EXPECT_NE(rest.find("HTTP/1.1 200"), std::string::npos) << "第二个请求应该得到响应";
}

// 审查 #8：控制面的连接数上限
TEST_F(ServerTest, control_plane_limits_concurrent_connections) {
  std::vector<std::unique_ptr<Client>> idle;
  for (int i = 0; i < 140; ++i)
    idle.push_back(std::make_unique<Client>(server_->observability_port()));
  int closed = 0;
  for (auto& c : idle) closed += c->closed_within(milliseconds(20)) ? 1 : 0;
  EXPECT_GE(closed, 12) << "超过 128 个的控制面连接应该被立即关闭";
  idle.clear();
  std::this_thread::sleep_for(milliseconds(200));
  Client health(server_->observability_port());
  health.send("GET /health HTTP/1.1\r\nHost: t\r\n\r\n");
  EXPECT_EQ(health.read_response().msg.result_int(), 200);
}

}  // namespace
}  // namespace llmgw::server::test

namespace llmgw::server::test {
namespace {

std::size_t open_fds() {
  std::size_t n = 0;
  for ([[maybe_unused]] const auto& e : std::filesystem::directory_iterator("/proc/self/fd")) ++n;
  return n;
}

// 资源边界：大量连接在各种阶段被突然断开之后，文件描述符要全部释放（不能有泄漏）
TEST_F(ServerTest, file_descriptors_are_released_after_abrupt_disconnects) {
  const std::size_t before = open_fds();
  {
    std::vector<std::unique_ptr<Client>> clients;
    for (int i = 0; i < 90; ++i) {
      auto c = std::make_unique<Client>(server_->data_port());
      switch (i % 3) {
        case 0:  // 只发了一半请求头
          c->send("POST /v1/chat/completions HTTP/1.1\r\nHost: t\r\n");
          break;
        case 1:  // 请求体发了一半
          c->send(
              "POST /v1/chat/completions HTTP/1.1\r\nHost: t\r\nContent-Length: "
              "1000\r\n\r\n{\"model\":");
          break;
        default:  // 正在流式输出
          c->post_chat("slow");
          break;
      }
      clients.push_back(std::move(c));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
  }  // 全部突然关闭
  for (int i = 0; i < 40 && open_fds() > before; ++i)
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  EXPECT_LE(open_fds(), before) << "突然断开 90 个连接之后，文件描述符没有全部释放";
  // 网关仍然正常工作
  Client c(server_->data_port());
  c.post_chat("ok");
  EXPECT_EQ(c.read_response().msg.result_int(), 200);
}

}  // namespace
}  // namespace llmgw::server::test
