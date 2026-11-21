#include <gtest/gtest.h>

#include "llmgw/config/loader.h"

namespace llmgw::config {
namespace {

TEST(FieldPath, parses_nested_and_each) {
  auto p = parse_field_path("messages[].reasoning_content");
  ASSERT_TRUE(p) << p.error().message;
  ASSERT_EQ(p->size(), 2U);
  EXPECT_EQ((*p)[0], (PathSegment{.name = "messages", .each = true}));
  EXPECT_EQ((*p)[1], (PathSegment{.name = "reasoning_content", .each = false}));
  EXPECT_EQ(to_string(*p), "messages[].reasoning_content");
}

TEST(FieldPath, accepts_single_name_with_dash_and_digits) {
  auto p = parse_field_path("x-custom_2");
  ASSERT_TRUE(p);
  EXPECT_EQ(to_string(*p), "x-custom_2");
}

TEST(FieldPath, rejects_bad_syntax_with_position) {
  for (const char* bad : {"", ".a", "a.", "a..b", "a[0].b", "a[]b", "a b", "a.[]"}) {
    SCOPED_TRACE(bad);
    EXPECT_FALSE(parse_field_path(bad));
  }
  auto r = parse_field_path("messages[0].x");
  ASSERT_FALSE(r);
  EXPECT_NE(r.error().message.find("第 9 个字符"), std::string::npos) << r.error().message;
}

TEST(Url, parses_scheme_host_default_port_and_path) {
  auto u = parse_url("https://api.openai.com/v1/");
  ASSERT_TRUE(u);
  EXPECT_EQ(*u,
            (Url{.scheme = "https", .host = "api.openai.com", .port = 443, .base_path = "/v1"}));
  auto h = parse_url("http://127.0.0.1:18081");
  ASSERT_TRUE(h);
  EXPECT_EQ(*h, (Url{.scheme = "http", .host = "127.0.0.1", .port = 18081, .base_path = ""}));
  auto v6 = parse_url("http://[::1]:8080/v1");
  ASSERT_TRUE(v6);
  EXPECT_EQ(v6->host, "::1");
  EXPECT_EQ(v6->port, 8080);
}

TEST(Url, rejects_unsupported_forms) {
  for (const char* bad :
       {"ftp://x", "api.openai.com/v1", "https://", "https://h:0", "https://h:70000",
        "https://h/v1?x=1", "https://h/#a", "https://user:pw@h/v1", "http://::1/v1"}) {
    SCOPED_TRACE(bad);
    EXPECT_FALSE(parse_url(bad));
  }
  auto r = parse_url("https://user:sk-secret@h/v1");
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().message.find("sk-secret"), std::string::npos) << "错误信息中不能出现密钥";
}

TEST(Endpoint, requires_host_and_port) {
  auto e = parse_endpoint("127.0.0.1:9090");
  ASSERT_TRUE(e);
  EXPECT_EQ(*e, (Endpoint{.host = "127.0.0.1", .port = 9090}));
  auto v6 = parse_endpoint("[::1]:9090");
  ASSERT_TRUE(v6);
  EXPECT_EQ(v6->host, "::1");
  for (const char* bad : {"127.0.0.1", ":9090", "h:abc", "h:99999", "[::1"}) {
    SCOPED_TRACE(bad);
    EXPECT_FALSE(parse_endpoint(bad));
  }
}

}  // namespace
}  // namespace llmgw::config
