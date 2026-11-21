// 每个 Worker、每个上游一个连接池。只在一个线程上使用，不加锁。
//
// “名额”（busy）= 借出的连接 + 正在建立的连接，不超过 limit。名额由 Lease 持有：
// Lease 析构时一定归还（RAII），所以被取消、在竞速中被丢弃的结果都不会泄漏名额。
// 归还时如果有排队者，名额（或者可复用的连接）直接交给队首的排队者，busy 不变。
#pragma once

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/steady_timer.hpp>
#include <cstddef>
#include <list>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "connection.h"
#include "llmgw/core/metrics.h"
#include "llmgw/core/task.h"

namespace llmgw::upstream::detail {

struct Waiter;

class Pool {
 public:
  Pool(std::string provider, std::size_t limit, MetricsRecorder* metrics)
      : provider_(std::move(provider)), limit_(limit), metrics_(metrics) {}
  Pool(const Pool&) = delete;
  Pool& operator=(const Pool&) = delete;
  Pool(Pool&&) = delete;
  Pool& operator=(Pool&&) = delete;
  ~Pool() = default;

  [[nodiscard]] const std::string& provider() const { return provider_; }
  [[nodiscard]] std::size_t idle_count() const { return idle_.size(); }
  [[nodiscard]] std::size_t busy_count() const { return busy_; }
  [[nodiscard]] std::size_t waiting_count() const { return waiters_.size(); }

  /// 名额用完时的结果：一个空闲连接（复用），或者一个空名额（由调用者新建连接）
  struct Grant {
    std::unique_ptr<Connection> conn;  ///< 为空表示新建
  };

  /// 不等待地取：有健康的空闲连接就复用，名额未满就给一个空名额；都没有时返回空。
  /// 取到时 busy 已经加一。
  std::optional<Grant> try_take();

  /// 归还名额（连接已经关闭或者没建成）
  void free_slot();
  /// 归还一条可以复用的连接
  void give_back(std::unique_ptr<Connection> conn, SteadyClock::time_point now);
  /// 关闭空闲超过 idle_timeout 的连接，以及已经被上游关闭的连接
  void sweep(SteadyClock::time_point now, SteadyClock::duration idle_timeout);
  /// 关闭所有空闲连接（Client 析构时）
  void close_idle() {
    idle_.clear();
    report();
  }

  // 排队（由 acquire() 使用）
  std::list<Waiter*>::iterator enqueue(Waiter* w) { return waiters_.insert(waiters_.end(), w); }
  void dequeue(std::list<Waiter*>::iterator it) { waiters_.erase(it); }

 private:
  void report();
  // 把名额（和可能的连接）交给队首的排队者；没有排队者时返回 false
  bool hand_to_waiter(std::unique_ptr<Connection>* conn);

  std::string provider_;
  std::size_t limit_;
  MetricsRecorder* metrics_;
  std::size_t busy_ = 0;
  std::vector<std::unique_ptr<Connection>> idle_;  ///< 后进先出：最近用过的在末尾
  std::list<Waiter*> waiters_;
};

/// 一个排队者。活在 acquire() 的协程帧里。
struct Waiter {
  explicit Waiter(const asio::any_io_executor& ex) : timer(ex, SteadyClock::time_point::max()) {}
  asio::steady_timer timer;  ///< 不会到期，被交接时取消，用来唤醒
  bool granted = false;
  std::unique_ptr<Connection> conn;  ///< 交接的是可复用的连接时不为空
};

/// 持有一个名额（以及可能的一条连接）。析构时：连接关闭，名额归还。
class Lease {
 public:
  Lease() = default;
  Lease(const std::shared_ptr<Pool>& pool, std::unique_ptr<Connection> conn)
      : pool_(pool), conn_(std::move(conn)), held_(true) {}
  Lease(Lease&& other) noexcept
      : pool_(std::move(other.pool_)),
        conn_(std::move(other.conn_)),
        held_(std::exchange(other.held_, false)) {}
  Lease& operator=(Lease&& other) noexcept {
    if (this != &other) {
      release();
      pool_ = std::move(other.pool_);
      conn_ = std::move(other.conn_);
      held_ = std::exchange(other.held_, false);
    }
    return *this;
  }
  Lease(const Lease&) = delete;
  Lease& operator=(const Lease&) = delete;
  ~Lease() { release(); }

  [[nodiscard]] bool held() const { return held_; }
  [[nodiscard]] Connection* conn() const { return conn_.get(); }
  void set_connection(std::unique_ptr<Connection> conn) { conn_ = std::move(conn); }
  /// 关闭当前连接，保留名额（换新连接重发时）
  void drop_connection() { conn_.reset(); }
  /// 连接可以复用：还回连接池（池已经不存在时关闭）
  void give_back(SteadyClock::time_point now);

 private:
  // 析构和移动赋值中调用：不能抛异常（指标上报、定时器取消理论上可能抛）
  void release() noexcept;
  std::weak_ptr<Pool> pool_;  ///< Client 先于 Lease 销毁时（Worker 退出），只关闭连接
  std::unique_ptr<Connection> conn_;
  bool held_ = false;
};

/// 从连接池取一个名额：没有可用的就排队，直到被交接或者被取消（排队的时限由调用者的竞速控制）。
/// 被取消时返回空。返回的 Lease 中，conn() 不为空表示复用，为空表示需要新建。
Task<std::optional<Lease>> acquire(std::shared_ptr<Pool> pool);

}  // namespace llmgw::upstream::detail
