// 指标上报接口 MetricsRecorder，以及指标目录。
//
// 依赖倒置（design.md 3.2）：接口在 core，实现在 observability（每个 Worker 一个分片），
// Worker 启动时注入。下层模块只依赖这个接口，不依赖 observability。
//
// 指标的名字、说明、标签名都在这里统一登记（observability.md 3.2、3.3），
// 防止各模块各自起名、随意加标签导致时间序列失控。
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <span>
#include <string_view>

namespace llmgw {

/// 计数器。只登记已经有模块上报的指标；新增时在枚举和 kCounterCatalog 中各加一行。
enum class Counter : std::uint8_t {
  kUpstreamAttempts,    ///< executor：每次上游尝试
  kUpstreamAuthErrors,  ///< executor：上游返回 401/403（上游 key 失效）
  kRejected,            ///< server 等：请求被拒绝（过载、过大 …）
  /// upstream：每次从连接池取连接的结果（kind = reused / new / queue_timeout），复用率由此算出
  kUpstreamConnections,
};

/// 直方图（固定分桶，由实现决定）。
enum class Histogram : std::uint8_t {
  kUpstreamDuration,  ///< executor：一次上游尝试的耗时，单位秒
};

/// 仪表（当前值）。
enum class Gauge : std::uint8_t {
  kActiveConnections,  ///< server：当前客户端连接数
  kActiveRequests,     ///< server：当前正在处理的请求数（按 Worker）
  kPoolConnections,    ///< upstream：连接池中的连接数（按上游、空闲/使用中）
};

/// 一个指标的登记信息。
struct MetricInfo {
  std::string_view name;                     ///< Prometheus 指标名
  std::string_view help;                     ///< 说明
  std::span<const std::string_view> labels;  ///< 标签名；上报时标签值的个数和顺序必须一致
};

/// 目录中的一项：自己对应的枚举值 + 登记信息。
template <class Id>
struct CatalogEntry {
  Id id;
  MetricInfo info;
};

namespace metric_labels {
inline constexpr std::array<std::string_view, 0> kNone{};
inline constexpr std::array<std::string_view, 1> kProvider{"provider"};
inline constexpr std::array<std::string_view, 2> kProviderResult{"provider", "result"};
inline constexpr std::array<std::string_view, 2> kProviderState{"provider", "state"};
inline constexpr std::array<std::string_view, 2> kProviderKind{"provider", "kind"};
inline constexpr std::array<std::string_view, 1> kReason{"reason"};
inline constexpr std::array<std::string_view, 1> kWorker{"worker"};
}  // namespace metric_labels

// 目录的顺序必须与枚举的顺序一致（下面的 static_assert 检查）。
inline constexpr std::array kCounterCatalog{
    CatalogEntry<Counter>{Counter::kUpstreamAttempts,
                          {"llmgw_upstream_attempts_total", "上游尝试次数，按结果分类",
                           metric_labels::kProviderResult}},
    CatalogEntry<Counter>{
        Counter::kUpstreamAuthErrors,
        {"llmgw_upstream_auth_errors_total", "上游返回鉴权错误的次数", metric_labels::kProvider}},
    CatalogEntry<Counter>{
        Counter::kRejected,
        {"llmgw_rejected_total", "被拒绝的请求数，按原因分类", metric_labels::kReason}},
    CatalogEntry<Counter>{
        Counter::kUpstreamConnections,
        {"llmgw_upstream_connections_total", "从连接池取连接的次数：复用、新建、排队超时",
         metric_labels::kProviderKind}},
};

inline constexpr std::array kHistogramCatalog{
    CatalogEntry<Histogram>{
        Histogram::kUpstreamDuration,
        {"llmgw_upstream_duration_seconds", "一次上游尝试的耗时", metric_labels::kProvider}},
};

inline constexpr std::array kGaugeCatalog{
    CatalogEntry<Gauge>{Gauge::kActiveConnections,
                        {"llmgw_active_connections", "当前客户端连接数", metric_labels::kNone}},
    CatalogEntry<Gauge>{Gauge::kActiveRequests,
                        {"llmgw_active_requests", "当前正在处理的请求数", metric_labels::kWorker}},
    CatalogEntry<Gauge>{
        Gauge::kPoolConnections,
        {"llmgw_pool_connections", "连接池中的连接数", metric_labels::kProviderState}},
};

namespace detail {
template <class Catalog>
constexpr bool ids_match_positions(const Catalog& catalog) {
  for (std::size_t i = 0; i < catalog.size(); ++i) {
    if (static_cast<std::size_t>(catalog[i].id) != i) return false;
  }
  return true;
}
}  // namespace detail

static_assert(detail::ids_match_positions(kCounterCatalog), "kCounterCatalog 的顺序与枚举不一致");
static_assert(detail::ids_match_positions(kHistogramCatalog),
              "kHistogramCatalog 的顺序与枚举不一致");
static_assert(detail::ids_match_positions(kGaugeCatalog), "kGaugeCatalog 的顺序与枚举不一致");

/// 查询登记信息。
constexpr const MetricInfo& info(Counter id) {
  return kCounterCatalog.at(static_cast<std::size_t>(id)).info;
}
constexpr const MetricInfo& info(Histogram id) {
  return kHistogramCatalog.at(static_cast<std::size_t>(id)).info;
}
constexpr const MetricInfo& info(Gauge id) {
  return kGaugeCatalog.at(static_cast<std::size_t>(id)).info;
}

/// 标签值，顺序与目录中登记的标签名一致。
using LabelValues = std::span<const std::string_view>;

/// 指标上报接口。
///
/// 用法：`rec.increment(Counter::kUpstreamAttempts, {provider, label(result)});`
///
/// 线程：每个 Worker 一个实例，只在该 Worker 的线程上调用，实现不需要加锁。
/// 频率：每次调用要查找一次标签组合。按请求、按上游尝试的频率没有问题；
///       **不要在每个数据块上调用**。
/// 失败：上报不能影响请求处理。标签值个数与目录不一致时丢弃这次上报，并计入
///       dropped()，由实现作为自身的指标输出；测试中可以断言它为 0。
///
/// 采用 NVI（公开非虚函数 + 私有虚函数）：标签个数的检查只写在这里一处，
/// 所有实现都自动得到。
class MetricsRecorder {
 public:
  MetricsRecorder() = default;
  MetricsRecorder(const MetricsRecorder&) = delete;
  MetricsRecorder& operator=(const MetricsRecorder&) = delete;
  MetricsRecorder(MetricsRecorder&&) = delete;
  MetricsRecorder& operator=(MetricsRecorder&&) = delete;
  virtual ~MetricsRecorder() = default;

  /// 计数器加 n。
  void increment(Counter id, std::initializer_list<std::string_view> labels, std::uint64_t n = 1) {
    increment(id, LabelValues(labels.begin(), labels.size()), n);
  }
  void increment(Counter id, LabelValues labels, std::uint64_t n = 1) {
    if (check(info(id), labels)) do_increment(id, labels, n);
  }
  /// 直方图记录一个值。
  void observe(Histogram id, std::initializer_list<std::string_view> labels, double value) {
    observe(id, LabelValues(labels.begin(), labels.size()), value);
  }
  void observe(Histogram id, LabelValues labels, double value) {
    if (check(info(id), labels)) do_observe(id, labels, value);
  }
  /// 仪表设为一个值。
  void set(Gauge id, std::initializer_list<std::string_view> labels, double value) {
    set(id, LabelValues(labels.begin(), labels.size()), value);
  }
  void set(Gauge id, LabelValues labels, double value) {
    if (check(info(id), labels)) do_set(id, labels, value);
  }

  /// 因标签个数不一致而丢弃的上报次数。
  [[nodiscard]] std::uint64_t dropped() const { return dropped_; }

 private:
  bool check(const MetricInfo& meta, LabelValues labels) {
    if (labels.size() == meta.labels.size()) return true;
    ++dropped_;
    return false;
  }

  virtual void do_increment(Counter id, LabelValues labels, std::uint64_t n) = 0;
  virtual void do_observe(Histogram id, LabelValues labels, double value) = 0;
  virtual void do_set(Gauge id, LabelValues labels, double value) = 0;

  std::uint64_t dropped_ = 0;
};

/// 什么都不做的实现：用于单元测试，以及还没有注入真实实现的场合。
class NullMetricsRecorder final : public MetricsRecorder {
 private:
  void do_increment(Counter /*id*/, LabelValues /*labels*/, std::uint64_t /*n*/) override {}
  void do_observe(Histogram /*id*/, LabelValues /*labels*/, double /*value*/) override {}
  void do_set(Gauge /*id*/, LabelValues /*labels*/, double /*value*/) override {}
};

}  // namespace llmgw
