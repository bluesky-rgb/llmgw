// SseParser：S1（任意切块，路线图 1.5 的验收标准）、S2（SSE
// 规范）、S4（结束）、S5（上限）、S9（不重复扫描）。
#include "llmgw/stream/sse_parser.h"

#include <gtest/gtest.h>

#include <chrono>
#include <random>
#include <string>
#include <vector>

namespace llmgw::stream {
namespace {

struct Parsed {
  std::vector<SseEvent> events;
  bool finished_ok = true;
  [[nodiscard]] std::string forwarded() const {
    std::string all;
    for (const auto& e : events) all += e.raw;
    return all;
  }
};

bool operator==(const SseEvent& a, const SseEvent& b) {
  return a.raw == b.raw && a.data == b.data && a.event == b.event && a.has_data == b.has_data;
}

// 按 cuts（切分位置，递增）把 text 切块喂入
Parsed parse(const std::string& text, const std::vector<std::size_t>& cuts) {
  SseParser p;
  Parsed out;
  std::size_t prev = 0;
  auto drain = [&] {
    while (auto e = p.next()) out.events.push_back(std::move(*e));
  };
  for (const std::size_t c : cuts) {
    p.feed(std::string_view(text).substr(prev, c - prev));
    drain();
    prev = c;
  }
  p.feed(std::string_view(text).substr(prev));
  drain();
  out.finished_ok = p.finish();
  drain();
  return out;
}

Parsed parse_whole(const std::string& text) { return parse(text, {}); }

// 有代表性的 SSE 文本：三家厂商的格式、各种换行、注释、多行 data、BOM、少了最后的空行等
const std::vector<std::string>& samples() {
  static const std::vector<std::string> s = {
      // OpenAI：每一块都有 "usage": null，最后一块只有 usage，最后是 [DONE]
      "data: "
      "{\"id\":\"c1\",\"choices\":[{\"index\":0,\"delta\":{\"content\":\"你好\"}}],\"usage\":null}"
      "\n\n"
      "data: "
      "{\"id\":\"c1\",\"choices\":[{\"index\":0,\"delta\":{\"content\":\"世界\"}}],\"usage\":null}"
      "\n\n"
      "data: "
      "{\"id\":\"c1\",\"choices\":[],\"usage\":{\"prompt_tokens\":9,\"completion_tokens\":2}}\n\n"
      "data: [DONE]\n\n",
      // CRLF、推理内容、注释心跳
      ": OPENROUTER PROCESSING\r\n\r\n"
      "data: {\"choices\":[{\"delta\":{\"reasoning_content\":\"想一想\"}}]}\r\n\r\n"
      "data: {\"choices\":[{\"delta\":{\"content\":\"好\"}}]}\r\n\r\n"
      "data: [DONE]\r\n\r\n",
      // 只用 CR
      "data: a\r\rdata: b\r\r",
      // 混合换行、多行 data、event/id/retry、没有空格的字段、没有冒号的行、未知字段
      "event: ping\nid: 7\nretry: 100\n\n"
      "data:x\ndata: y\r\ndata\rfoo: bar\n\n"
      ":comment only\n\n",
      // 开头的 BOM
      "\xEF\xBB\xBF"
      "data: bom\n\n",
      // 少了最后的空行（有的上游这样结束）
      "data: 1\n\ndata: [DONE]\n",
      // 以 \r 结束（最后的 \r 是不是 \r\n 的前一半，只有到结尾才知道）
      "data: last\r\n\r",
      // 空的 data
      "data:\n\ndata\n\n",
  };
  return s;
}

TEST(SseParser, S1_every_two_way_split_gives_identical_results) {
  for (const auto& text : samples()) {
    const Parsed whole = parse_whole(text);
    ASSERT_TRUE(whole.finished_ok) << text;
    for (std::size_t cut = 0; cut <= text.size(); ++cut) {
      const Parsed p = parse(text, {cut});
      ASSERT_EQ(p.finished_ok, whole.finished_ok) << "cut=" << cut;
      ASSERT_EQ(p.events.size(), whole.events.size()) << "cut=" << cut << " in: " << text;
      for (std::size_t i = 0; i < p.events.size(); ++i)
        ASSERT_TRUE(p.events[i] == whole.events[i]) << "cut=" << cut;
    }
  }
}

TEST(SseParser, S1_byte_by_byte_and_random_splits_give_identical_results) {
  std::mt19937 rng(20261121);
  for (const auto& text : samples()) {
    const Parsed whole = parse_whole(text);
    std::vector<std::size_t> every;
    for (std::size_t i = 1; i < text.size(); ++i) every.push_back(i);
    const Parsed bytes = parse(text, every);
    ASSERT_EQ(bytes.events.size(), whole.events.size());
    for (std::size_t i = 0; i < bytes.events.size(); ++i)
      ASSERT_TRUE(bytes.events[i] == whole.events[i]);
    for (int round = 0; round < 12500; ++round) {  // 8 个样本 × 12500 = 10 万次
      std::vector<std::size_t> cuts;
      for (std::size_t i = 1; i < text.size(); ++i) {
        if (rng() % 4 == 0) cuts.push_back(i);
      }
      const Parsed p = parse(text, cuts);
      ASSERT_EQ(p.events.size(), whole.events.size());
      for (std::size_t i = 0; i < p.events.size(); ++i) ASSERT_TRUE(p.events[i] == whole.events[i]);
      ASSERT_EQ(p.forwarded(), whole.forwarded());
    }
  }
}

TEST(SseParser, S1_forwarded_bytes_are_exactly_the_input) {
  for (const auto& text : samples()) {
    const Parsed whole = parse_whole(text);
    std::string expect = text;
    // 唯一的改动：结束时补上缺少的空行
    if (text == "data: 1\n\ndata: [DONE]\n") expect += "\n";
    EXPECT_EQ(whole.forwarded(), expect);
  }
}

// ---------- S2：SSE 规范 ----------

TEST(SseParser, S2_field_rules) {
  const Parsed p = parse_whole(samples()[3]);
  ASSERT_EQ(p.events.size(), 3U);
  EXPECT_EQ(p.events[0].event, "ping");
  EXPECT_FALSE(p.events[0].has_data);
  EXPECT_EQ(p.events[1].data, "x\ny\n") << "冒号后的一个空格不属于值；没有冒号的 data 行是空值";
  EXPECT_TRUE(p.events[1].has_data);
  EXPECT_FALSE(p.events[2].has_data) << "只有注释的事件";
  EXPECT_EQ(p.events[2].raw, ":comment only\n\n");
}

TEST(SseParser, S2_line_endings_and_bom) {
  const Parsed cr = parse_whole(samples()[2]);
  ASSERT_EQ(cr.events.size(), 2U);
  EXPECT_EQ(cr.events[0].data, "a");
  EXPECT_EQ(cr.events[1].data, "b");
  const Parsed bom = parse_whole(samples()[4]);
  ASSERT_EQ(bom.events.size(), 1U);
  EXPECT_EQ(bom.events[0].data, "bom") << "BOM 不属于字段名";
  EXPECT_TRUE(bom.events[0].raw.starts_with("\xEF\xBB\xBF")) << "转发时保留";
  const Parsed empty = parse_whole(samples()[7]);
  ASSERT_EQ(empty.events.size(), 2U);
  EXPECT_TRUE(empty.events[0].has_data);
  EXPECT_EQ(empty.events[0].data, "");
}

TEST(SseParser, S2_only_one_space_after_colon_is_removed) {
  const Parsed p = parse_whole("data:  two spaces\n\n");
  ASSERT_EQ(p.events.size(), 1U);
  EXPECT_EQ(p.events[0].data, " two spaces");
}

// ---------- S4：结束 ----------

TEST(SseParser, S4_missing_final_blank_line_is_completed) {
  const Parsed p = parse_whole("data: 1\n\ndata: [DONE]\n");
  ASSERT_TRUE(p.finished_ok);
  ASSERT_EQ(p.events.size(), 2U);
  EXPECT_EQ(p.events[1].data, "[DONE]");
  EXPECT_EQ(p.events[1].raw, "data: [DONE]\n\n");
  const Parsed crlf = parse_whole("data: x\r\n");
  ASSERT_EQ(crlf.events.size(), 1U);
  EXPECT_EQ(crlf.events[0].raw, "data: x\r\n\r\n");
}

TEST(SseParser, S4_truncated_line_is_an_error_not_half_an_event) {
  const Parsed p = parse_whole("data: 1\n\ndata: {\"choices\":[{\"del");
  EXPECT_FALSE(p.finished_ok);
  ASSERT_EQ(p.events.size(), 1U) << "已经完整的事件照常交出，半个事件不交出";
}

TEST(SseParser, S4_trailing_cr_is_resolved_at_the_end) {
  const Parsed p = parse_whole("data: last\r\n\r");
  ASSERT_TRUE(p.finished_ok);
  ASSERT_EQ(p.events.size(), 1U);
  EXPECT_EQ(p.events[0].raw, "data: last\r\n\r");
  const Parsed two = parse_whole("data: a\r\r");
  ASSERT_EQ(two.events.size(), 1U) << "结尾的 \\r 构成空行：事件在 finish 时交出";
}

TEST(SseParser, S4_empty_input) {
  const Parsed p = parse_whole("");
  EXPECT_TRUE(p.finished_ok);
  EXPECT_TRUE(p.events.empty());
}

// ---------- S5：上限 ----------

TEST(SseParser, S5_event_without_blank_line_is_capped) {
  SseParser p(1024);
  for (int i = 0; i < 100 && !p.overflowed(); ++i) p.feed("data: " + std::string(100, 'x') + "\n");
  EXPECT_TRUE(p.overflowed());
  EXPECT_LE(p.buffered(), 1024U + 200U) << "超过上限后不再增长";
  EXPECT_FALSE(p.finish());
}

TEST(SseParser, S5_oversized_event_is_rejected_however_it_arrives) {
  // 模糊测试发现：超大事件一次性到达时曾被放行，分成小块到达时被拒绝
  const std::string ev = "data: " + std::string(2000, 'x') + "\n\n";
  for (const bool whole : {true, false}) {
    SCOPED_TRACE(whole ? "一次到达" : "逐字节到达");
    SseParser p(1024);
    if (whole) {
      p.feed(ev);
    } else {
      for (const char c : ev) p.feed(std::string_view(&c, 1));
    }
    EXPECT_TRUE(p.overflowed());
    EXPECT_FALSE(p.next().has_value());
  }
}

TEST(SseParser, S5_many_events_do_not_accumulate_memory) {
  SseParser p(1024);
  for (int i = 0; i < 100000; ++i) {
    p.feed("data: " + std::to_string(i) + "\n\n");
    while (p.next()) {
    }
  }
  EXPECT_FALSE(p.overflowed()) << "已经交出的事件不计入上限";
  EXPECT_EQ(p.buffered(), 0U);
}

// ---------- S9：不重复扫描 ----------

TEST(SseParser, S9_large_event_arriving_byte_by_byte_is_linear) {
  // 1 MB 的事件逐字节到达：如果每次都从事件开头重新扫描，是 O(n²)（约 5000 亿次比较）
  const std::string line = "data: " + std::string(1 << 20, 'x') + "\n\n";
  SseParser p(std::size_t{4} << 20);
  const auto t0 = std::chrono::steady_clock::now();
  for (const char c : line) p.feed(std::string_view(&c, 1));
  const auto elapsed = std::chrono::steady_clock::now() - t0;
  ASSERT_TRUE(p.next().has_value());
  EXPECT_LT(elapsed, std::chrono::seconds(5));
  std::printf("  [1 MB 事件逐字节喂入：%lld ms]\n",
              static_cast<long long>(
                  std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count()));
}

}  // namespace
}  // namespace llmgw::stream
