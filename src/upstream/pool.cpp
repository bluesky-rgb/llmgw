#include "pool.h"

#include <algorithm>
#include <boost/asio/as_tuple.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <cstdio>

namespace llmgw::upstream::detail {

std::optional<Pool::Grant> Pool::try_take() {
  while (!idle_.empty()) {
    std::unique_ptr<Connection> conn = std::move(idle_.back());
    idle_.pop_back();
    if (conn->healthy()) {
      ++busy_;
      report();
      return Grant{std::move(conn)};
    }
    // 已被上游关闭：析构时关闭，取下一条
  }
  report();
  if (busy_ >= limit_) return std::nullopt;
  ++busy_;
  report();
  return Grant{nullptr};
}

bool Pool::hand_to_waiter(std::unique_ptr<Connection>* conn) {
  if (waiters_.empty()) return false;
  Waiter* w = waiters_.front();
  waiters_.pop_front();
  w->granted = true;
  if (conn != nullptr) w->conn = std::move(*conn);
  w->timer.cancel();  // 唤醒；名额随之转移，busy 不变
  return true;
}

void Pool::free_slot() {
  if (hand_to_waiter(nullptr)) return;
  if (busy_ > 0) --busy_;
  report();
}

void Pool::give_back(std::unique_ptr<Connection> conn, SteadyClock::time_point now) {
  conn->idle_since = now;
  conn->buffer().shrink_to_fit();  // 空闲连接不占读缓冲区（缓冲区此时一定是空的）
  if (hand_to_waiter(&conn)) return;
  if (busy_ > 0) --busy_;
  idle_.push_back(std::move(conn));
  report();
}

void Pool::sweep(SteadyClock::time_point now, SteadyClock::duration idle_timeout) {
  std::erase_if(idle_, [&](const std::unique_ptr<Connection>& c) {
    return now - c->idle_since >= idle_timeout || !c->healthy();
  });
  report();
}

void Pool::report() {
  if (metrics_ == nullptr) return;
  metrics_->set(Gauge::kPoolConnections, {provider_, "idle"}, static_cast<double>(idle_.size()));
  metrics_->set(Gauge::kPoolConnections, {provider_, "busy"}, static_cast<double>(busy_));
}

void Lease::give_back(SteadyClock::time_point now) {
  if (!held_) return;
  held_ = false;
  if (auto pool = pool_.lock(); pool && conn_) {
    pool->give_back(std::move(conn_), now);
  } else {
    conn_.reset();
  }
}

void Lease::release() noexcept {
  if (!held_) return;
  held_ = false;
  try {
    conn_.reset();  // 先关闭连接，再归还名额
    if (auto pool = pool_.lock()) pool->free_slot();
  } catch (...) {
    // 析构中不能抛出：最多少还一个名额，好过进程终止
    std::fputs("[upstream] releasing a connection slot failed\n", stderr);
  }
}

namespace {

// 协程帧被销毁（取消后返回，或者事件循环退出时帧被直接销毁）时，保证排队者离开队列
// 已经被交接（granted）的排队者已经不在队列中，交接给它的名额（或连接）要还回去（独立审查 R2：
// 曾经重复出队，并且泄漏名额）
struct QueueGuard {
  QueueGuard(std::shared_ptr<Pool> p, Waiter* waiter, std::list<Waiter*>::iterator i)
      : pool(std::move(p)), w(waiter), it(i) {}
  std::shared_ptr<Pool> pool;
  Waiter* w;
  std::list<Waiter*>::iterator it;
  bool queued = true;
  QueueGuard(const QueueGuard&) = delete;
  QueueGuard& operator=(const QueueGuard&) = delete;
  QueueGuard(QueueGuard&&) = delete;
  QueueGuard& operator=(QueueGuard&&) = delete;
  ~QueueGuard() {
    if (!queued) return;
    try {
      if (!w->granted) {
        pool->dequeue(it);
      } else if (w->conn) {
        pool->give_back(std::move(w->conn), SteadyClock::now());
      } else {
        pool->free_slot();
      }
    } catch (...) {
      std::fputs("[upstream] returning a granted slot failed\n", stderr);
    }
  }
};

}  // namespace

Task<std::optional<Lease>> acquire(std::shared_ptr<Pool> pool) {
  if (auto g = pool->try_take()) co_return Lease(pool, std::move(g->conn));
  const auto ex = co_await asio::this_coro::executor;
  Waiter w(ex);
  QueueGuard guard(pool, &w, pool->enqueue(&w));
  co_await w.timer.async_wait(asio::as_tuple(asio::use_awaitable));
  if (!w.granted) co_return std::nullopt;  // 被取消（排队超时、客户端断开）：guard 负责出队
  guard.queued = false;                    // 交接时已经出队
  // 交接给我们的名额已经计入 busy。即使此时我们也被取消了，返回的 Lease 被丢弃时会把名额交给下一位
  if (w.conn && !w.conn->healthy()) w.conn.reset();  // 交接的连接刚还回来，一般是健康的
  co_return Lease(pool, std::move(w.conn));
}

}  // namespace llmgw::upstream::detail
