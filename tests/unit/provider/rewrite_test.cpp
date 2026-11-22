// 请求体改写（任务 1.6）：P1 三种改写（路线图验收项）、P2 边界、P3 内置改写、P4 请求头与路径、P5
// 不合法的请求体、P9 性能。
#include "rewrite.h"

#include <gtest/gtest.h>

#include <chrono>
#include <nlohmann/json.hpp>

#include "llmgw/config/loader.h"
#include "llmgw/provider/provider.h"

namespace llmgw::provider {
namespace {

using detail::rewrite;
using detail::Rules;
using Json = nlohmann::json;

config::FieldPath path(std::string_view text) { return *config::parse_field_path(text); }

Rules rules(std::vector<std::string> drops = {},
            std::vector<std::pair<std::string, std::string>> renames = {},
            std::vector<std::pair<std::string, std::string>> sets = {},
            bool include_usage = false) {
  Rules r;
  r.model = "gpt-5.6-sol";
  r.include_usage = include_usage;
  for (const auto& d : drops) r.drops.push_back(path(d));
  for (const auto& [f, t] : renames) r.renames.push_back({.from = path(f), .to = path(t)});
  for (const auto& [p, v] : sets) r.sets.push_back({.path = path(p), .json_value = v});
  return r;
}

// JSON 的 Unicode 转义（反斜杠 + u + 4
// 位十六进制），运行时拼出来：避免编辑工具把源码中的转义还原成字符
std::string u_escape(std::string_view hex) { return std::string(1, '\\') + "u" + std::string(hex); }

Json run(std::string_view body, const Rules& r) {
  auto out = rewrite(body, r);
  EXPECT_TRUE(out.has_value()) << (out ? "" : out.error().message);
  if (!out) return nullptr;
  return Json::parse(out->body);
}

// ---------- P1：三种改写 ----------

TEST(Rewrite, P1_drop_fields_top_level_and_each) {
  const Json out = run(
      R"({"model":"smart","thinking":{"type":"enabled"},"messages":[
           {"role":"user","content":"hi","reasoning_content":"r1"},
           {"role":"assistant","content":"yo","reasoning_content":"r2"},
           "not an object", {"role":"user"}]})",
      rules({"thinking", "messages[].reasoning_content", "absent", "messages[].absent"}));
  EXPECT_FALSE(out.contains("thinking"));
  ASSERT_EQ(out["messages"].size(), 4U);
  EXPECT_EQ(out["messages"][0], (Json{{"role", "user"}, {"content", "hi"}}));
  EXPECT_EQ(out["messages"][1], (Json{{"role", "assistant"}, {"content", "yo"}}));
  EXPECT_EQ(out["messages"][2], "not an object") << "不是对象的元素原样保留";
  EXPECT_EQ(out["model"], "gpt-5.6-sol");
}

TEST(Rewrite, P1_rename_fields_including_existing_target) {
  const Json out =
      run(R"({"model":"m","max_tokens":100,"max_completion_tokens":5,
                           "messages":[{"role":"user","a":1},{"role":"user","b":9,"a":2}]})",
          rules({}, {{"max_tokens", "max_completion_tokens"}, {"messages[].a", "messages[].b"}}));
  EXPECT_EQ(out["max_completion_tokens"], 100) << "改名的结果覆盖原来的同名项";
  EXPECT_FALSE(out.contains("max_tokens"));
  EXPECT_EQ(out["messages"][0], (Json{{"role", "user"}, {"b", 1}}));
  EXPECT_EQ(out["messages"][1], (Json{{"role", "user"}, {"b", 2}}));
}

TEST(Rewrite, P1_set_fields_replace_create_and_nested) {
  const Json out = run(R"({"model":"m","reasoning_effort":"high","extra":5})",
                       rules({}, {},
                             {{"reasoning_effort", R"("medium")"},
                              {"thinking.type", R"("disabled")"},
                              {"extra.inner", "1"},
                              {"obj", R"({"x":[1,2]})"}}));
  EXPECT_EQ(out["reasoning_effort"], "medium");
  EXPECT_EQ(out["thinking"], (Json{{"type", "disabled"}})) << "中间的对象一起创建";
  EXPECT_EQ(out["extra"], (Json{{"inner", 1}})) << "路径经过的值不是对象：替换为对象";
  EXPECT_EQ(out["obj"], (Json{{"x", {1, 2}}}));
}

TEST(Rewrite, P1_set_inside_each_element) {
  const Json out = run(R"({"model":"m","messages":[{"role":"user"},{"role":"tool","name":"x"}]})",
                       rules({}, {}, {{"messages[].name", R"("n")"}}));
  EXPECT_EQ(out["messages"][0]["name"], "n");
  EXPECT_EQ(out["messages"][1]["name"], "n");
  const Json none = run(R"({"model":"m"})", rules({}, {}, {{"messages[].name", R"("n")"}}));
  EXPECT_FALSE(none.contains("messages")) << "不会创建数组：没有元素可以设置";
}

TEST(Rewrite, P1_order_is_drop_then_rename_then_set) {
  // 删除 a 后又设置 a：结果是设置的值；改名 b → c 后设置 c：结果是设置的值
  const Json out =
      run(R"({"model":"m","a":1,"b":2})", rules({"a"}, {{"b", "c"}}, {{"a", "10"}, {"c", "20"}}));
  EXPECT_EQ(out, (Json{{"model", "gpt-5.6-sol"}, {"a", 10}, {"c", 20}}));
}

// ---------- P2：边界 ----------

TEST(Rewrite, P2_untouched_values_are_byte_identical) {
  // 超过 64 位的整数、指数、转义、Unicode、空白：DOM 往返会改变它们，这里必须逐字节保留
  const std::string untouched = R"("big":123456789012345678901234567890,"exp":1.0E+2,"esc":"a)" +
                                u_escape("0041") + R"(\n\"q\"","cn":"你好",")" + u_escape("006b") +
                                R"(":1,"nested":{ "k" : [ 1 , 2 ] })";
  ASSERT_NE(untouched.find(u_escape("006b")), std::string::npos);
  auto out = rewrite(R"({"model":"m",)" + untouched + "}", rules());
  ASSERT_TRUE(out.has_value());
  EXPECT_EQ(out->body, R"({"model":"gpt-5.6-sol",)" + untouched + "}");
}

TEST(Rewrite, P2_type_mismatches_are_skipped) {
  const Json out = run(R"({"model":"m","messages":"oops","thinking":[1]})",
                       rules({"messages[].reasoning_content", "thinking.type"}));
  EXPECT_EQ(out["messages"], "oops");
  EXPECT_EQ(out["thinking"], Json::array({1}));
}

TEST(Rewrite, P2_duplicate_and_escaped_keys) {
  auto out = rewrite(R"({"model":"m","a":1,"a":2,")" + u_escape("0062") + R"(":3})",
                     rules({"b"}, {}, {{"a", "9"}}));
  ASSERT_TRUE(out.has_value());
  EXPECT_EQ(out->body, R"({"model":"gpt-5.6-sol","a":9,"a":9})")
      << "转义的键按解码后的名字匹配；每个同名项都替换";
}

TEST(Rewrite, P2_model_with_special_characters_is_escaped) {
  Rules r = rules();
  r.model = "a\"b\\c\n";
  const Json out = run(R"({"model":"m"})", r);
  EXPECT_EQ(out["model"], "a\"b\\c\n");
}

TEST(Rewrite, P2_empty_object_and_whitespace) {
  const Json out = run("  \n{ }\n ", rules());
  EXPECT_EQ(out, (Json{{"model", "gpt-5.6-sol"}}));
}

TEST(Rewrite, P2_untouched_parts_are_only_scanned) {
  // 已知限制（README）：没有被改写路径经过的值只做结构扫描，其中不合法的 JSON 原样转发，由上游返回
  // 400
  auto out = rewrite(R"({"model":"m","x":[tru,01]})", rules());
  ASSERT_TRUE(out.has_value());
  EXPECT_EQ(out->body, R"({"model":"gpt-5.6-sol","x":[tru,01]})");
  // 改写路径经过的对象：结构不对（缺少值、多余的逗号）时报错
  EXPECT_FALSE(
      rewrite(R"({"model":"m","messages":[{"a":}]})", rules({"messages[].a"})).has_value());
  EXPECT_FALSE(
      rewrite(R"({"model":"m","messages":[{"a":1,}]})", rules({"messages[].a"})).has_value());
}

TEST(Rewrite, P2_deep_nesting_outside_rules_is_not_recursed) {
  // 递归只沿着规则树：客户端的嵌套深度不影响栈（10 万层）
  const std::string deep = std::string(100000, '[') + std::string(100000, ']');
  auto out = rewrite(R"({"model":"m","x":)" + deep + "}", rules({"messages[].a"}));
  // simdjson 的默认深度上限是 1024：超过时报错（400），也不会耗尽栈
  if (out) {
    EXPECT_EQ(out->body, R"({"model":"gpt-5.6-sol","x":)" + deep + "}");
  } else {
    EXPECT_EQ(out.error().code, ErrorCode::kInvalidRequest);
  }
  const std::string ok_deep = std::string(500, '[') + std::string(500, ']');
  auto fine = rewrite(R"({"model":"m","x":)" + ok_deep + "}", rules());
  ASSERT_TRUE(fine.has_value());
}

// ---------- P3：内置改写 ----------

TEST(Rewrite, P3_include_usage_injection) {
  struct Case {
    const char* body;
    bool injected;
  };
  for (const Case c :
       {Case{R"({"model":"m","stream":true})", true},
        Case{R"({"model":"m","stream":true,"stream_options":null})", true},
        Case{R"({"model":"m","stream":true,"stream_options":{"x":1}})", true},
        Case{R"({"model":"m","stream":true,"stream_options":{"include_usage":false}})", true},
        Case{R"({"model":"m","stream":true,"stream_options":{"include_usage":true}})", false}}) {
    SCOPED_TRACE(c.body);
    auto out = rewrite(c.body, rules({}, {}, {}, true));
    ASSERT_TRUE(out.has_value());
    const Json j = Json::parse(out->body);
    EXPECT_EQ(j["stream_options"]["include_usage"], true);
    EXPECT_EQ(out->injected_include_usage, c.injected)
        << "客户端自己要求了 usage 时不能去掉 usage 块";
  }
  // 审查 1.6 #1：转义的键、重复的键（任意一项为 true 即视为客户端要求了）
  for (const std::string& body : std::vector<std::string>{
           R"({"model":"m","stream_options":{"include)" + u_escape("005f") + R"(usage":true}})",
           R"({"model":"m","stream_options":{"include_usage":false,"include_usage":true}})",
           R"({"model":"m",")" + u_escape("0073") + R"(tream_options":{"include_usage":true}})"}) {
    SCOPED_TRACE(body);
    auto out = rewrite(body, rules({}, {}, {}, true));
    ASSERT_TRUE(out.has_value());
    EXPECT_FALSE(out->injected_include_usage);
  }
  const Json keep = run(R"({"model":"m","stream_options":{"x":1}})", rules({}, {}, {}, true));
  EXPECT_EQ(keep["stream_options"]["x"], 1) << "保留 stream_options 的其他字段";
}

TEST(Rewrite, P3_config_overrides_builtin) {
  // 不支持 stream_options 的上游：用 drop_fields 去掉网关加的
  auto out = rewrite(R"({"model":"m","stream":true})", rules({"stream_options"}, {}, {}, true));
  ASSERT_TRUE(out.has_value());
  EXPECT_FALSE(Json::parse(out->body).contains("stream_options"));
  EXPECT_FALSE(out->injected_include_usage) << "没有加上，就不能去掉 usage 块";

  // 审查 1.6 #5：配置自己设置了 include_usage = true，客户端没有要求：同样要去掉 usage 块
  auto forced =
      rewrite(R"({"model":"m"})", rules({}, {}, {{"stream_options.include_usage", "true"}}, true));
  ASSERT_TRUE(forced.has_value());
  EXPECT_TRUE(forced->injected_include_usage);
  auto whole = rewrite(R"({"model":"m"})",
                       rules({}, {}, {{"stream_options", R"({"include_usage":true})"}}, true));
  ASSERT_TRUE(whole.has_value());
  EXPECT_TRUE(whole->injected_include_usage);
  auto off =
      rewrite(R"({"model":"m"})", rules({}, {}, {{"stream_options.include_usage", "false"}}, true));
  ASSERT_TRUE(off.has_value());
  EXPECT_FALSE(off->injected_include_usage);
  // 配置的路径在内置改写的路径之下：同样让给配置
  auto below =
      rewrite(R"({"model":"m"})", rules({}, {}, {{"stream_options.include_usage.x", "1"}}, true));
  ASSERT_TRUE(below.has_value());
  EXPECT_EQ(Json::parse(below->body)["stream_options"]["include_usage"], (Json{{"x", 1}}));
  EXPECT_FALSE(below->injected_include_usage);

  // 改名 stream_options：内置改写不能再创建一个 stream_options
  const Json renamed = run(R"({"model":"m","stream":true,"stream_options":{"a":1}})",
                           rules({}, {{"stream_options", "so"}}, {}, true));
  EXPECT_EQ(renamed, (Json{{"model", "gpt-5.6-sol"}, {"stream", true}, {"so", {{"a", 1}}}}));
}

TEST(Rewrite, P3_non_stream_request_is_left_alone) {
  auto out = rewrite(R"({"model":"m","stream_options":{"include_usage":false}})", rules());
  ASSERT_TRUE(out.has_value());
  EXPECT_EQ(Json::parse(out->body)["stream_options"]["include_usage"], false);
  EXPECT_FALSE(out->injected_include_usage);
}

// ---------- P4：请求头与路径 ----------

TEST(Prepare, P4_headers_path_and_body) {
  config::Provider p;
  p.name = "openai";
  p.base_url = {.scheme = "https", .host = "api.openai.com", .port = 443, .base_path = "/v1"};
  p.api_key = "sk-SECRET";
  config::TargetConfig t;
  t.provider = "openai";
  t.model = "gpt-5.6-sol";
  RequestInfo req;
  req.model = "smart";
  req.stream = true;
  req.body = R"({"model":"smart","stream":true,"messages":[]})";
  auto prepared = prepare(req, p, t);
  ASSERT_TRUE(prepared.has_value());
  EXPECT_EQ(prepared->request.target, "/v1/chat/completions");
  EXPECT_EQ(prepared->request.method, "POST");
  EXPECT_TRUE(prepared->stream);
  EXPECT_TRUE(prepared->strip_usage_chunk);
  auto header = [&](std::string_view name) {
    for (const auto& h : prepared->request.headers) {
      if (h.name == name) return h.value;
    }
    return std::string();
  };
  EXPECT_EQ(header("Authorization"), "Bearer sk-SECRET");
  EXPECT_EQ(header("Content-Type"), "application/json");
  EXPECT_EQ(header("Accept"), "text/event-stream");
  EXPECT_TRUE(header("User-Agent").starts_with("llmgw/"));
  EXPECT_EQ(Json::parse(prepared->request.body)["model"], "gpt-5.6-sol");

  p.base_url.base_path = "";
  req.body = "[1]";
  EXPECT_EQ(prepare(req, p, t).error().code, ErrorCode::kInvalidRequest);
  auto bad = prepare(req, p, t);
  EXPECT_EQ(bad.error().message.find("SECRET"), std::string::npos);
}

// ---------- P7：响应头白名单 ----------

TEST(ResponseHeaders, P7_allowlist_and_request_id) {
  const std::vector<Header> in{{"Content-Type", "text/event-stream"},
                               {"Set-Cookie", "session=abc"},
                               {"Transfer-Encoding", "chunked"},
                               {"Connection", "keep-alive"},
                               {"Cache-Control", "no-cache"},
                               {"Retry-After", "3"},
                               {"retry-after-ms", "3000"},
                               {"x-should-retry", "false"},
                               {"x-request-id", "req-1"},
                               {"openai-organization", "org-x"},
                               {"Content-Length", "10"}};
  const std::vector<Header> expected{{"Content-Type", "text/event-stream"},
                                     {"Cache-Control", "no-cache"},
                                     {"Retry-After", "3"},
                                     {"retry-after-ms", "3000"},
                                     {"x-should-retry", "false"},
                                     {"X-LLMGW-Upstream-Request-Id", "req-1"}};
  EXPECT_EQ(filter_response_headers(in), expected);
  EXPECT_EQ(filter_response_headers({{"Request-Id", "r2"}}),
            (std::vector<Header>{{"X-LLMGW-Upstream-Request-Id", "r2"}}));
  // 审查 1.6 #10：两个都有时只转发第一个
  EXPECT_EQ(filter_response_headers({{"x-request-id", "a"}, {"request-id", "b"}}),
            (std::vector<Header>{{"X-LLMGW-Upstream-Request-Id", "a"}}));
}

// ---------- P5：不合法的请求体 ----------

TEST(Rewrite, P5_invalid_bodies_are_400) {
  for (const char* body :
       {"", "[]", "\"s\"", "{", R"({"model":)", R"({"model":"m"} trailing)",
        R"({"model":"m"} {"x":1})", R"({"model":"m"}})", R"({"model":"m"},)", R"({"model":"m"} 1)",
        R"({"model":"m","messages":[{"content":"x"},]})", "nul"}) {
    SCOPED_TRACE(body);
    auto out = rewrite(body, rules({"messages[].reasoning_content"}));
    ASSERT_FALSE(out.has_value());
    EXPECT_EQ(out.error().code, ErrorCode::kInvalidRequest);
  }
}

// ---------- P9：性能 ----------

#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__) || !defined(NDEBUG)
constexpr bool kMeasurable = false;
#else
constexpr bool kMeasurable = true;
#endif

TEST(Rewrite, P9_cost_by_body_size) {
  for (const std::size_t messages : {4U, 100U, 1000U}) {
    std::string body = R"({"model":"smart","stream":true,"temperature":0.7,"messages":[)";
    for (std::size_t i = 0; i < messages; ++i) {
      if (i > 0) body += ",";
      body += R"({"role":"user","content":")" + std::string(900, 'a') +
              R"(","reasoning_content":"think"})";
    }
    body += "]}";
    const Rules r = rules({"messages[].reasoning_content"}, {}, {}, true);
    const int n = messages >= 1000 ? 10 : 100;
    double ms = 1e9;  // 5 批中最快的一批：排除共享机器上的偶发干扰
    for (int batch = 0; batch < 5; ++batch) {
      const auto t0 = std::chrono::steady_clock::now();
      for (int i = 0; i < n; ++i) ASSERT_TRUE(rewrite(body, r).has_value());
      ms = std::min(
          ms,
          std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() /
              n);
    }
    std::printf("  [%zu KB：%.3f ms]\n", body.size() / 1024, ms);
    if (kMeasurable && messages == 1000) {
      EXPECT_LT(ms, 1.0) << "1 MB 的请求体应该在 1 ms 以内（DOM 方案约 10 ms）";
    }
  }
}

}  // namespace
}  // namespace llmgw::provider
