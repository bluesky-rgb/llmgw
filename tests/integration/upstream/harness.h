// upstream 测试的公共部分：在测试线程上运行 Client、构造配置、记录指标。
#pragma once

#include <gtest/gtest.h>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <chrono>
#include <map>
#include <memory>
#include <optional>
#include <string>

#include "llmgw/config/snapshot.h"
#include "llmgw/core/metrics.h"
#include "llmgw/upstream/client.h"
#include "test_servers.h"
#include "tls_certs.h"

namespace llmgw::test {

namespace asio = boost::asio;
using namespace std::chrono_literals;
using upstream::AttemptError;
using upstream::Client;
using upstream::Stage;

/// 在 ioc 上运行一个协程直到它结束（最多 limit）。超时说明有 bug（比如卡死），测试失败。
template <class T>
T run(asio::io_context& ioc, Task<T> task, std::chrono::milliseconds limit = 10s) {
  std::optional<T> out;
  std::exception_ptr error;
  asio::co_spawn(ioc, std::move(task), [&](std::exception_ptr e, T v) {
    error = e;
    out.emplace(std::move(v));
  });
  const auto deadline = std::chrono::steady_clock::now() + limit;
  ioc.restart();
  while (!out && std::chrono::steady_clock::now() < deadline) ioc.run_one_for(20ms);
  if (error) std::rethrow_exception(error);
  if (!out) throw std::runtime_error("协程没有在时限内结束（卡住了？）");
  return std::move(*out);
}

/// 让 ioc 处理一段时间（让后台的协程执行，比如空闲连接清理）
inline void pump_for(asio::io_context& ioc, std::chrono::milliseconds d) {
  ioc.restart();
  ioc.run_for(d);
}

/// 记录型的指标
class RecordingMetrics final : public MetricsRecorder {
 public:
  std::map<std::string, std::uint64_t> counters;  ///< "名字{标签,…}" → 值
  std::map<std::string, double> gauges;
  std::uint64_t counter(Counter id, std::initializer_list<std::string_view> labels) const {
    auto it = counters.find(key(info(id).name, labels));
    return it == counters.end() ? 0 : it->second;
  }
  double gauge(Gauge id, std::initializer_list<std::string_view> labels) const {
    auto it = gauges.find(key(info(id).name, labels));
    return it == gauges.end() ? -1 : it->second;
  }

 private:
  static std::string key(std::string_view name, std::span<const std::string_view> labels) {
    std::string k(name);
    k += "{";
    for (auto l : labels) k += std::string(l) + ",";
    return k + "}";
  }
  static std::string key(std::string_view name, std::initializer_list<std::string_view> labels) {
    return key(name, std::span<const std::string_view>(labels.begin(), labels.size()));
  }
  void do_increment(Counter id, LabelValues labels, std::uint64_t n) override {
    counters[key(info(id).name, labels)] += n;
  }
  void do_observe(Histogram, LabelValues, double) override {}
  void do_set(Gauge id, LabelValues labels, double value) override {
    gauges[key(info(id).name, labels)] = value;
  }
};

/// 一个上游的配置
inline config::Provider provider(const std::string& scheme, const std::string& host,
                                 std::uint16_t port, std::uint32_t max_connections = 256) {
  config::Provider p;
  p.name = "up";
  p.base_url = config::Url{.scheme = scheme, .host = host, .port = port, .base_path = ""};
  p.api_key_env = "K";
  p.max_connections = max_connections;
  return p;
}

/// 完整读完的响应
struct Fetched {
  int status = 0;
  std::vector<Header> headers;
  std::string body;
  std::optional<AttemptError> error;  ///< send 或读响应体时的失败
  [[nodiscard]] bool ok() const { return !error.has_value(); }
};

inline upstream::Request post(std::string body = R"({"model":"m"})") {
  upstream::Request r;
  r.target = "/v1/chat/completions";
  r.headers = {{"Content-Type", "application/json"}, {"Authorization", "Bearer sk-test"}};
  r.body = std::move(body);
  return r;
}

inline upstream::Deadlines deadlines(std::chrono::milliseconds connect = 2s,
                                     std::chrono::milliseconds first = 2s) {
  return {connect, first};
}

/// 发送并读完整个响应体
inline Task<Fetched> fetch(Client* client, const config::Provider* p, upstream::Request req,
                           upstream::Deadlines d = deadlines()) {
  Fetched f;
  auto r = co_await client->send(p, std::move(req), d);
  if (!r) {
    f.error = r.error();
    co_return f;
  }
  f.status = r->status;
  f.headers = r->headers;
  for (;;) {
    auto more = co_await r->body->read(&f.body);
    if (!more) {
      f.error = more.error();
      break;
    }
    if (!*more) break;
  }
  co_return f;
}

/// 每个测试一份：io_context、配置、Client
// 成员的声明顺序就是销毁顺序的反序：client 最先销毁，然后是 ioc（挂起的协程帧此时被销毁，
// 还会上报指标、访问配置），最后才是配置和指标（独立审查 #9：原来的顺序让指标先于 ioc 被销毁）
struct Env {
  RecordingMetrics metrics;
  std::shared_ptr<config::Snapshot> config = std::make_shared<config::Snapshot>();
  asio::io_context ioc{1};
  std::unique_ptr<Client> client;

  /// 添加上游并创建 Client（TLS 上下文按配置创建）
  const config::Provider* start(config::Provider p, upstream::ClientOptions extra = {}) {
    config->providers.push_back(std::move(p));
    auto tls = upstream::make_tls_contexts(*config);
    if (!tls) throw std::runtime_error(tls.error().message);
    extra.executor = ioc.get_executor();
    extra.config = config;
    extra.tls = *tls;
    extra.metrics = &metrics;
    client = std::make_unique<Client>(std::move(extra));
    return &config->providers.back();
  }
  Fetched get(const config::Provider* p, upstream::Request req = post(),
              upstream::Deadlines d = deadlines()) {
    return run(ioc, fetch(client.get(), p, std::move(req), d));
  }
};

}  // namespace llmgw::test
