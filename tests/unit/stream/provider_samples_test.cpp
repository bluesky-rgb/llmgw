// 真实上游的响应样本（任务 1.8）：tests/data/providers/ 中的每个样本都经过
// stream::open()，检查不变式。 样本由 tools/provider-probe/probe.py 录制；samples/index.json
// 记录每个样本的状态码和 Content-Type。 环境变量 LLMGW_PROVIDER_SAMPLES
// 可以指向另一个样本目录（验证这个测试本身时使用）。
#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <sstream>

#include "stream_test_util.h"

namespace llmgw::stream {
namespace {

namespace fs = std::filesystem;
using Json = nlohmann::json;

fs::path samples_root() {
  if (const char* env = std::getenv("LLMGW_PROVIDER_SAMPLES"); env != nullptr && *env != '\0')
    return env;
  return LLMGW_PROVIDER_SAMPLES_DIR;
}

std::string read_file(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  std::ostringstream s;
  s << in.rdbuf();
  return s.str();
}

struct Sample {
  std::string name;  ///< <上游>/<文件名>
  std::string provider;
  int status = 0;
  std::string content_type;
  std::string body;
};

std::vector<Sample> load() {
  std::vector<Sample> out;
  const fs::path index = samples_root() / "index.json";
  if (!fs::exists(index)) return out;
  const Json j = Json::parse(read_file(index));
  for (const auto& [name, meta] : j.items()) {
    Sample s;
    s.name = name;
    s.provider = name.substr(0, name.find('/'));
    s.status = meta.at("status").get<int>();
    s.content_type = meta.at("content_type").get<std::string>();
    s.body = read_file(samples_root() / name);
    out.push_back(std::move(s));
  }
  return out;
}

// 1 字节一块：检验结果与分块方式无关（SSE 解析器、JSON 检查的边界情况）
std::vector<Step> bytewise(const std::string& body) {
  std::vector<Step> steps;
  steps.reserve(body.size());
  for (char c : body) steps.push_back({std::string(1, c)});
  return steps;
}

// 样本中最后一个 usage 的 prompt_tokens（没有时返回 -1）
std::int64_t last_prompt_tokens(const std::vector<Json>& events) {
  std::int64_t v = -1;
  for (const auto& e : events) {
    if (e.is_object() && e.contains("usage") && e["usage"].is_object() &&
        e["usage"].contains("prompt_tokens"))
      v = e["usage"]["prompt_tokens"].get<std::int64_t>();
  }
  return v;
}

// SSE 的事件（以空行分隔）。返回每个事件的原文和它 data: 的 JSON（不是 JSON 时为 null）
std::vector<std::pair<std::string, Json>> sse_events(const std::string& body) {
  std::vector<std::pair<std::string, Json>> out;
  std::size_t pos = 0;
  while (pos < body.size()) {
    std::size_t end = body.find("\n\n", pos);
    end = end == std::string::npos ? body.size() : end + 2;
    std::string raw = body.substr(pos, end - pos);
    Json data = nullptr;
    if (auto d = raw.find("data:"); d != std::string::npos) {
      std::string payload = raw.substr(d + 5);
      while (!payload.empty() && (payload.back() == '\n' || payload.back() == '\r'))
        payload.pop_back();
      if (!payload.empty() && payload.front() == ' ') payload.erase(0, 1);
      data = Json::parse(payload, nullptr, false);
      if (data.is_discarded()) data = nullptr;
    }
    out.emplace_back(std::move(raw), std::move(data));
    pos = end;
  }
  return out;
}

ResponseHead head_of(const Sample& s) {
  return ResponseHead{.status = s.status, .headers = {{"Content-Type", s.content_type}}};
}

TEST(ProviderSamples, every_sample_passes_through_the_stream_layer) {
  const auto samples = load();
  if (samples.empty())
    GTEST_SKIP() << "没有样本：" << samples_root() << "（任务 1.8 由作者用真实 key 录制）";
  int sse = 0;
  for (const auto& s : samples) {
    SCOPED_TRACE(s.name);
    const bool is_sse = s.content_type.find("text/event-stream") != std::string::npos;
    // 1. 不去掉 usage 块：逐字节原样转发；整块输入与逐字节输入的结果相同
    auto ctx = make_ctx();
    const Drained whole = run(drain(head_of(s), {{s.body}}, &ctx));
    ASSERT_FALSE(whole.error.has_value()) << whole.error->message;
    EXPECT_EQ(whole.status, s.status);
    EXPECT_EQ(whole.all(), s.body) << "没有改写的响应必须原样转发";
    auto ctx2 = make_ctx();
    const Drained bytes = run(drain(head_of(s), bytewise(s.body), &ctx2));
    EXPECT_EQ(bytes.all(), s.body) << "结果不能依赖分块方式";
    if (!is_sse || s.status != 200) continue;
    ++sse;
    // 2. 用量：与样本中最后一个 usage 一致
    std::vector<Json> datas;
    for (const auto& [raw, data] : sse_events(s.body)) datas.push_back(data);
    const std::int64_t prompt = last_prompt_tokens(datas);
    EXPECT_EQ(ctx.usage.reported, prompt >= 0);
    if (prompt >= 0) {
      EXPECT_EQ(ctx.usage.prompt_tokens, prompt);
    }
    // 3. 去掉 usage 块：只少了“choices 为空、带 usage”的事件，其余原样
    std::string expected;
    for (const auto& [raw, data] : sse_events(s.body)) {
      const bool usage_only = data.is_object() && data.contains("usage") &&
                              data["usage"].is_object() &&
                              (!data.contains("choices") || data["choices"].empty());
      if (!usage_only) expected += raw;
    }
    auto ctx3 = make_ctx();
    const Drained stripped =
        run(drain(head_of(s), {{s.body}}, &ctx3, Options{.strip_usage_chunk = true}));
    EXPECT_EQ(stripped.all(), expected);
    EXPECT_EQ(ctx3.usage.reported, prompt >= 0) << "去掉 usage 块时仍然要记录用量";
  }
  std::printf("  [%zu 个样本，其中 %d 个成功的流式响应]\n", samples.size(), sse);
}

// MiniMax 的业务错误码（providers.md 3.4 的映射）对真实的响应体有效
TEST(ProviderSamples, minimax_business_errors_are_mapped) {
  const auto samples = load();
  if (samples.empty()) GTEST_SKIP() << "没有样本";
  const ErrorMapping mapping{
      .path = {"base_resp", "status_code"},
      .codes = {{"1002", 429}, {"1004", 401}, {"1008", 402}, {"1001", 504}, {"1013", 500}}};
  int checked = 0;
  for (const auto& s : samples) {
    if (s.provider != "minimax" || s.status != 200) continue;
    SCOPED_TRACE(s.name);
    std::string code;
    if (s.content_type.find("text/event-stream") != std::string::npos) {
      const auto events = sse_events(s.body);
      for (const auto& [raw, data] : events) {
        if (data.is_object()) {  // 只检查第一个 JSON 事件（与 stream 的预读一致）
          if (data.contains("base_resp"))
            code = data["base_resp"].value("status_code", Json(0)).dump();
          break;
        }
      }
    } else {
      const Json j = Json::parse(s.body, nullptr, false);
      if (j.is_object() && j.contains("base_resp"))
        code = j["base_resp"].value("status_code", Json(0)).dump();
    }
    const auto it = mapping.codes.find(code);
    const int expected = it == mapping.codes.end() ? 200 : it->second;
    auto ctx = make_ctx();
    const Drained d = run(drain(head_of(s), {{s.body}}, &ctx, Options{.error_mapping = &mapping}));
    EXPECT_EQ(d.status, expected) << "base_resp.status_code = " << code;
    ++checked;
  }
  std::printf("  [检查了 %d 个 MiniMax 的样本]\n", checked);
}

}  // namespace
}  // namespace llmgw::stream
