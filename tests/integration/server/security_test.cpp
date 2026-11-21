// 安全：请求走私（分帧有歧义的请求必须拒绝）、响应头注入、畸形输入。任务 1.3 之后的排查中加入。
#include "fixture.h"

namespace llmgw::server::test {
namespace {

std::string chat_body(const std::string& model = "ok") {
  return R"({"model":")" + model + R"(","stream":true})";
}

std::string with_length(const std::string& head_lines, const std::string& body) {
  return "POST /v1/chat/completions HTTP/1.1\r\nHost: t\r\n" + head_lines +
         "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
}

std::string chunked(const std::string& te, const std::string& body) {
  char size[16];
  std::snprintf(size, sizeof(size), "%zx", body.size());
  return "POST /v1/chat/completions HTTP/1.1\r\nHost: t\r\nTransfer-Encoding: " + te + "\r\n\r\n" +
         size + "\r\n" + body + "\r\n0\r\n\r\n";
}

// 发送原始请求，返回状态码（没有收到响应时为 0），并检查之后连接被关闭
int status_then_closed(std::uint16_t port, const std::string& raw, bool* closed) {
  Client c(port);
  c.send(raw);
  auto r = c.read_response();
  *closed = c.closed_within(std::chrono::milliseconds(1500));
  return r.ok ? r.msg.result_int() : 0;
}

// 分帧有歧义的请求：前面的反向代理和网关可能对“请求体在哪里结束”理解不同，
// 多出来的字节会被当作下一个请求（请求走私）。必须返回 400 并关闭连接，不能交给 Dispatcher。
TEST_F(ServerTest, ambiguous_framing_is_rejected_and_connection_closed) {
  const auto body = chat_body();
  struct Case {
    const char* name;
    std::string raw;
  };
  const Case cases[] = {
      {"Content-Length 与 Transfer-Encoding 同时出现",
       "POST /v1/chat/completions HTTP/1.1\r\nHost: t\r\nContent-Length: 4\r\nTransfer-Encoding: "
       "chunked\r\n\r\n" +
           chunked("chunked", body).substr(chunked("chunked", body).find("\r\n\r\n") + 4)},
      {"两个不同的 Content-Length", with_length("Content-Length: 5\r\n", body)},
      {"chunked 不是最后一个编码", chunked("chunked, identity", body)},
      {"不支持的传输编码 gzip", chunked("gzip, chunked", body)},
      {"未知的传输编码", chunked("x-custom", body)},
      {"冒号前有空格", "POST /v1/chat/completions HTTP/1.1\r\nHost: t\r\nContent-Length : " +
                           std::to_string(body.size()) + "\r\n\r\n" + body},
      {"只有 LF 的换行", "POST /v1/chat/completions HTTP/1.1\nHost: t\nContent-Length: " +
                             std::to_string(body.size()) + "\n\n" + body},
  };
  for (const auto& c : cases) {
    SCOPED_TRACE(c.name);
    bool closed = false;
    EXPECT_EQ(status_then_closed(server_->data_port(), c.raw, &closed), 400);
    EXPECT_TRUE(closed) << "分帧有歧义时必须关闭连接，不能继续读取“下一个请求”";
  }
  EXPECT_EQ(probe_->dispatches, 0) << "有歧义的请求不应该被处理";
}

TEST_F(ServerTest, plain_chunked_request_body_is_accepted) {
  Client c(server_->data_port());
  c.send(chunked("chunked", chat_body()));
  auto r = c.read_response();
  ASSERT_TRUE(r.ok);
  EXPECT_EQ(r.msg.result_int(), 200);
}

// 上游返回的响应头中带 CR/LF 或非法字符：不能原样转发（否则可以向客户端注入 Set-Cookie
// 等任意响应头）
TEST_F(ServerTest, upstream_headers_cannot_inject_response_headers) {
  Client c(server_->data_port());
  c.send(with_length("Connection: close\r\n", chat_body("inject")));
  const std::string raw = c.read_until_close();
  const auto head_end = raw.find("\r\n\r\n");
  ASSERT_NE(head_end, std::string::npos) << raw;
  const std::string head = raw.substr(0, head_end);
  EXPECT_EQ(head.find("Set-Cookie"), std::string::npos) << "响应头注入成功：\n" << head;
  EXPECT_EQ(head.find("x-bad name"), std::string::npos) << "非法的响应头名字被转发了：\n" << head;
  EXPECT_NE(head.find("x-upstream: fake"), std::string::npos) << "合法的响应头应该照常转发";
  EXPECT_EQ(raw.rfind("HTTP/1.1 200", 0), 0U) << "响应应该照常返回";
}

// RFC 9112 3.2.2：服务端必须接受绝对形式的请求目标
TEST_F(ServerTest, absolute_form_target_is_accepted) {
  Client c(server_->data_port());
  const auto body = chat_body();
  c.send(
      "POST http://gateway.local/v1/chat/completions HTTP/1.1\r\nHost: "
      "gateway.local\r\nContent-Length: " +
      std::to_string(body.size()) + "\r\n\r\n" + body);
  auto r = c.read_response();
  ASSERT_TRUE(r.ok);
  EXPECT_EQ(r.msg.result_int(), 200);
}

TEST_F(ServerTest, nul_byte_in_body_is_rejected) {
  Client c(server_->data_port());
  const std::string body("{\"model\":\"o\0k\",\"stream\":true}", 30);
  c.send(with_length("", body));
  auto r = c.read_response();
  ASSERT_TRUE(r.ok);
  EXPECT_EQ(r.msg.result_int(), 400);
}

}  // namespace
}  // namespace llmgw::server::test
