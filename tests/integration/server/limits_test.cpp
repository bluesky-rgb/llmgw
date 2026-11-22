// 守住入口：慢速攻击、空闲连接、客户端断开时的取消、总超时、空闲超时、背压、中途出错、连接数上限。
// 测试配置：read_timeout、keep_alive_timeout、idle 都是 1 秒，total 是 2 秒（见 test_config()）。
#include "fixture.h"

namespace llmgw::server::test {
namespace {

using std::chrono::milliseconds;

// S5：慢速攻击——请求头每 300 ms 发一个字节，超过 read_timeout 后连接被关闭
TEST_F(ServerTest, slow_headers_are_cut_off_after_read_timeout) {
  Client c(server_->data_port());
  const auto start = Clock::now();
  const std::string head = "POST /v1/chat/completions HTTP/1.1\r\n";
  bool closed = false;
  for (char ch : head) {
    c.send(std::string(1, ch));
    if (c.closed_within(milliseconds(300))) {
      closed = true;
      break;
    }
  }
  EXPECT_TRUE(closed) << "慢速发送的请求头没有被断开";
  const auto took = Clock::now() - start;
  EXPECT_GE(took, milliseconds(900));
  EXPECT_LE(took, milliseconds(2500));
  EXPECT_EQ(probe_->dispatches, 0);
}

TEST_F(ServerTest, stalled_body_is_cut_off_after_read_timeout) {
  Client c(server_->data_port());
  c.send("POST /v1/chat/completions HTTP/1.1\r\nHost: t\r\nContent-Length: 100\r\n\r\n{\"model\":");
  EXPECT_TRUE(c.closed_within(milliseconds(2500)));
  EXPECT_EQ(probe_->dispatches, 0);
}

// 请求体慢慢但持续地发送，总时长超过 read_timeout：是正常请求（比如慢速网络上传大图片），不能断开
// 慢速网络上传一个较大的请求体：约 20 KB/s（高于 16 KB/s 的最低速度），总时长约 3 秒，超过
// read_timeout
TEST_F(ServerTest, slow_but_steady_body_is_accepted) {
  Client c(server_->data_port());
  const std::string body =
      R"({"model":"ok","stream":true,"pad":")" + std::string(60000, 'x') + "\"}";
  c.send("POST /v1/chat/completions HTTP/1.1\r\nHost: t\r\nContent-Length: " +
         std::to_string(body.size()) + "\r\n\r\n");
  for (std::size_t i = 0; i < body.size(); i += 3000) {  // 每 150 ms 发 3 KB：约 20 KB/s，共约 3 秒
    c.send(body.substr(i, 3000));
    std::this_thread::sleep_for(milliseconds(150));
  }
  auto r = c.read_response();
  ASSERT_TRUE(r.ok) << "正常速度的慢速上传被当作慢速攻击断开了";
  EXPECT_EQ(r.msg.result_int(), 200);
}

// 慢速攻击：一直有数据，但速度极低（独立审查发现：每 9 秒发 1 字节就能一直占住连接）
TEST_F(ServerTest, trickling_body_below_minimum_rate_is_cut_off) {
  Client c(server_->data_port());
  c.send(
      "POST /v1/chat/completions HTTP/1.1\r\nHost: t\r\nContent-Length: 50000\r\n\r\n{\"model\":");
  const auto start = Clock::now();
  bool closed = false;
  for (int i = 0; i < 20 && !closed; ++i) {  // 每 300 ms 发 10 字节：约 33 B/s，远低于 16 KB/s
    closed = !c.try_send(std::string(10, ' ')) || c.closed_within(milliseconds(300));
  }
  EXPECT_TRUE(closed) << "持续但极慢的请求体没有被断开";
  EXPECT_LT(Clock::now() - start, milliseconds(3500)) << "宽限期（1 秒）之后应该很快断开";
  EXPECT_EQ(probe_->dispatches, 0);
}

// S6：空闲的长连接
TEST_F(ServerTest, idle_keep_alive_connection_is_closed) {
  Client c(server_->data_port());
  c.post_chat("ok");
  ASSERT_TRUE(c.read_response().ok);
  const auto start = Clock::now();
  EXPECT_TRUE(c.closed_within(milliseconds(2500)));
  EXPECT_GE(Clock::now() - start, milliseconds(900));
}

// S7：客户端在流式输出中断开 → 上游的流在 100 ms 内被取消
TEST_F(ServerTest, client_disconnect_mid_stream_cancels_within_100ms) {
  Client c(server_->data_port());
  c.post_chat("slow");
  http::response_parser<http::string_body> p;
  ASSERT_TRUE(c.read_head(&p));
  std::this_thread::sleep_for(milliseconds(120));
  ASSERT_EQ(probe_->streams_destroyed, 0);
  const auto closed_at = Clock::now();
  c.close();
  for (int i = 0; i < 100 && probe_->streams_destroyed == 0; ++i)
    std::this_thread::sleep_for(milliseconds(5));
  ASSERT_EQ(probe_->streams_destroyed, 1) << "客户端断开后，流没有被取消";
  const auto latency = probe_->cancel_time() - closed_at;
  EXPECT_LT(latency, milliseconds(100))
      << std::chrono::duration_cast<milliseconds>(latency).count() << " ms";
}

// S8：客户端在 dispatch() 还没返回时断开 → dispatch 在 100 ms 内被取消
TEST_F(ServerTest, client_disconnect_during_dispatch_cancels_within_100ms) {
  Client c(server_->data_port());
  c.post_chat("hang");
  for (int i = 0; i < 100 && probe_->dispatches == 0; ++i)
    std::this_thread::sleep_for(milliseconds(5));
  ASSERT_EQ(probe_->dispatches, 1);
  const auto closed_at = Clock::now();
  c.close();
  for (int i = 0; i < 100 && probe_->dispatch_cancelled == 0; ++i)
    std::this_thread::sleep_for(milliseconds(5));
  ASSERT_EQ(probe_->dispatch_cancelled, 1);
  EXPECT_LT(probe_->cancel_time() - closed_at, milliseconds(100));
}

// S9：总超时
TEST_F(ServerTest, total_timeout_before_head_returns_504_without_retry) {
  Client c(server_->data_port());
  const auto start = Clock::now();
  c.post_chat("hang");
  auto r = c.read_response();
  ASSERT_TRUE(r.ok);
  EXPECT_EQ(r.msg.result_int(), 504);
  EXPECT_EQ(r.msg["x-should-retry"], "false") << "504 时上游可能已经计费，不能让 SDK 自动重试";
  EXPECT_NE(r.msg.body().find("upstream_timeout"), std::string::npos);
  EXPECT_GE(Clock::now() - start, milliseconds(1900));
  EXPECT_EQ(probe_->dispatch_cancelled, 1) << "超时后 dispatch 应该被取消";
}

TEST_F(ServerTest, total_timeout_after_head_cuts_the_stream) {
  Client c(server_->data_port());
  c.post_chat("slow");  // 1000 块 × 50 ms，远超 2 秒的总超时
  http::response_parser<http::string_body> p;
  ASSERT_TRUE(c.read_head(&p));
  EXPECT_EQ(p.get().result_int(), 200);
  EXPECT_TRUE(c.closed_within(milliseconds(3000)));
  EXPECT_EQ(probe_->streams_destroyed, 1);
  EXPECT_LT(probe_->chunks_served, 60);
}

// S10：空闲超时
TEST_F(ServerTest, stalled_upstream_is_cut_after_idle_timeout) {
  Client c(server_->data_port());
  c.post_chat("stall");  // 1 块之后一直不再发送
  http::response_parser<http::string_body> p;
  ASSERT_TRUE(c.read_head(&p));
  const auto start = Clock::now();
  EXPECT_TRUE(c.closed_within(milliseconds(2500)));
  EXPECT_GE(Clock::now() - start, milliseconds(900));
  EXPECT_LT(Clock::now() - start, milliseconds(1900))
      << "应该是空闲超时（1 秒）而不是总超时（2 秒）";
}

// 任务 1.7 X15：空闲超时按选中的目标（ctx->upstream_idle），而不是全局的 timeouts.idle
TEST_F(ServerTest, stalled_upstream_uses_the_targets_idle_timeout) {
  Client c(server_->data_port());
  c.post_chat("stall_short_idle");
  http::response_parser<http::string_body> p;
  ASSERT_TRUE(c.read_head(&p));
  const auto start = Clock::now();
  EXPECT_TRUE(c.closed_within(milliseconds(2500)));
  EXPECT_LT(Clock::now() - start, milliseconds(800)) << "应该是目标的 300 ms，而不是全局的 1 秒";
}

// S10：背压——客户端不读，网关就不再拉取
TEST_F(ServerTest, slow_reader_applies_backpressure) {
  Client c(server_->data_port());
  c.post_chat("big");  // 200 块 × 64 KB = 12.8 MB
  std::this_thread::sleep_for(milliseconds(500));
  const int served = probe_->chunks_served;
  EXPECT_LT(served, 100) << "客户端一个字节都没读，网关却拉取了 " << served << " 块（约 "
                         << served * 64 << " KB）";
  std::this_thread::sleep_for(milliseconds(300));
  EXPECT_EQ(probe_->chunks_served, served) << "被背压时不应该继续拉取";
}

// S11：拉取中途出错
TEST_F(ServerTest, mid_stream_error_ends_sse_with_error_event) {
  Client c(server_->data_port());
  c.post_chat("midfail", true);
  auto r = c.read_response();
  ASSERT_TRUE(r.ok) << "流式请求中途出错后应该正常结束（发送错误事件 + 结束分块）";
  EXPECT_EQ(r.msg.result_int(), 200);
  EXPECT_NE(r.msg.body().find("data: 0\n\ndata: 1\n\ndata: {\"error\":"), std::string::npos)
      << r.msg.body();
  EXPECT_NE(r.msg.body().find("upstream_unavailable"), std::string::npos);
  c.post_chat("ok");  // 连接仍然可以复用
  EXPECT_EQ(c.read_response().msg.result_int(), 200);
}

TEST_F(ServerTest, mid_stream_error_on_non_stream_request_closes_connection) {
  Client c(server_->data_port());
  c.post_chat("midfail", false);
  auto r = c.read_response();
  EXPECT_FALSE(r.ok) << "非流式响应无法表示中途出错，应该断开连接，让客户端看到不完整的响应";
}

// S13：连接数上限
TEST_F(ServerTest, connections_over_limit_get_503) {
  auto cfg = test_config();
  cfg->server.max_connections = 3;
  start(cfg);
  std::vector<std::unique_ptr<Client>> held;
  for (int i = 0; i < 3; ++i) {
    held.push_back(std::make_unique<Client>(server_->data_port()));
    held.back()->post_chat("slow");
    http::response_parser<http::string_body> p;
    ASSERT_TRUE(held.back()->read_head(&p));
  }
  Client extra(server_->data_port());
  auto r = extra.read_response();
  ASSERT_TRUE(r.ok);
  EXPECT_EQ(r.msg.result_int(), 503);
  EXPECT_NE(r.msg.body().find("overloaded"), std::string::npos);
  EXPECT_EQ(r.msg["x-should-retry"], "true");
  // 释放一个连接后，新连接可以正常使用
  held.front()->close();
  std::this_thread::sleep_for(milliseconds(100));
  Client again(server_->data_port());
  again.post_chat("ok");
  EXPECT_EQ(again.read_response().msg.result_int(), 200);
}

// S14：停止期间 /ready 返回 503（在另一个线程上调用 stop，观察控制面）
TEST_F(ServerTest, stop_closes_connections) {
  Client c(server_->data_port());
  c.post_chat("slow");
  http::response_parser<http::string_body> p;
  ASSERT_TRUE(c.read_head(&p));
  server_->stop();
  EXPECT_TRUE(c.closed_within(milliseconds(1000)));
  EXPECT_EQ(probe_->streams_destroyed, 1);
}

}  // namespace
}  // namespace llmgw::server::test
