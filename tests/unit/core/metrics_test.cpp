#include "llmgw/core/metrics.h"

#include <gtest/gtest.h>

#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace llmgw {
namespace {

template <class Catalog>
void expect_well_formed(const Catalog& catalog, std::string_view suffix,
                        std::set<std::string_view>* names) {
  for (const auto& entry : catalog) {
    const MetricInfo& m = entry.info;
    SCOPED_TRACE(std::string(m.name));
    EXPECT_TRUE(m.name.starts_with("llmgw_"));
    if (!suffix.empty()) {
      EXPECT_TRUE(m.name.ends_with(suffix));
    }
    EXPECT_FALSE(m.help.empty());
    EXPECT_TRUE(names->insert(m.name).second) << "指标名重复";
    for (auto l : m.labels) {
      EXPECT_FALSE(l.empty());
      // observability.md 3.3：禁止高基数标签
      EXPECT_NE(l, "request_id");
      EXPECT_NE(l, "key_id");
    }
  }
}

TEST(MetricsCatalog, names_are_unique_prefixed_and_suffixed) {
  std::set<std::string_view> names;
  expect_well_formed(kCounterCatalog, "_total", &names);
  expect_well_formed(kHistogramCatalog, "_seconds", &names);
  expect_well_formed(kGaugeCatalog, "", &names);
}

TEST(MetricsCatalog, lookup_returns_registered_info) {
  EXPECT_EQ(info(Counter::kUpstreamAttempts).name, "llmgw_upstream_attempts_total");
  ASSERT_EQ(info(Counter::kUpstreamAttempts).labels.size(), 2U);
  EXPECT_EQ(info(Counter::kUpstreamAttempts).labels[1], "result");
  EXPECT_EQ(info(Gauge::kActiveConnections).labels.size(), 0U);
}

// 记录每次调用的测试替身，用来验证基类的标签个数检查
class RecordingRecorder final : public MetricsRecorder {
 public:
  std::vector<std::string> calls;

 private:
  void do_increment(Counter id, LabelValues labels, std::uint64_t n) override {
    calls.push_back(std::string(info(id).name) + " " + join(labels) + " +" + std::to_string(n));
  }
  void do_observe(Histogram id, LabelValues labels, double /*value*/) override {
    calls.push_back(std::string(info(id).name) + " " + join(labels));
  }
  void do_set(Gauge id, LabelValues labels, double /*value*/) override {
    calls.push_back(std::string(info(id).name) + " " + join(labels));
  }
  static std::string join(LabelValues labels) {
    std::string s;
    for (auto l : labels) s += std::string(l) + ",";
    return s;
  }
};

TEST(MetricsRecorder, forwards_reports_with_matching_labels) {
  RecordingRecorder rec;
  rec.increment(Counter::kUpstreamAttempts, {"openai", "ok"});
  rec.increment(Counter::kRejected, {"too_large"}, 3);
  rec.observe(Histogram::kUpstreamDuration, {"openai"}, 0.25);
  rec.set(Gauge::kActiveConnections, {}, 10);
  EXPECT_EQ(rec.calls, (std::vector<std::string>{
                           "llmgw_upstream_attempts_total openai,ok, +1",
                           "llmgw_rejected_total too_large, +3",
                           "llmgw_upstream_duration_seconds openai,",
                           "llmgw_active_connections ",
                       }));
  EXPECT_EQ(rec.dropped(), 0U);
}

TEST(MetricsRecorder, drops_reports_with_wrong_label_count) {
  RecordingRecorder rec;
  rec.increment(Counter::kUpstreamAttempts, {"openai"});            // 少一个
  rec.observe(Histogram::kUpstreamDuration, {"openai", "x"}, 1.0);  // 多一个
  EXPECT_TRUE(rec.calls.empty());
  EXPECT_EQ(rec.dropped(), 2U);
}

TEST(NullMetricsRecorder, accepts_everything_silently) {
  NullMetricsRecorder rec;
  rec.increment(Counter::kUpstreamAttempts, {"openai", "ok"});
  rec.set(Gauge::kPoolConnections, {"openai", "idle"}, 4);
  EXPECT_EQ(rec.dropped(), 0U);
}

}  // namespace
}  // namespace llmgw
