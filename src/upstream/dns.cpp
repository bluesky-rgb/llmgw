#include "dns.h"

#include <boost/asio/as_tuple.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <chrono>
#include <exception>
#include <memory>
#include <optional>
#include <utility>

namespace llmgw::upstream::detail {

namespace asio = boost::asio;
using tcp = asio::ip::tcp;
using Endpoints = std::vector<tcp::endpoint>;

namespace {

constexpr auto kNegativeTtl = std::chrono::seconds(1);

// 一次正在进行的解析：所有等待者等同一个定时器，解析结束时取消它，唤醒全部等待者
struct Pending {
  explicit Pending(const asio::any_io_executor& ex) : done(ex, SteadyClock::time_point::max()) {}
  asio::steady_timer done;
  std::optional<Result<Endpoints>> result;
};

struct Entry {
  Endpoints endpoints;  ///< 为空表示这是一个失败结果
  Error error;
  SteadyClock::time_point expires;
};

}  // namespace

struct DnsCache::State {
  Resolver resolver;
  SteadyClock::duration ttl{};
  std::map<std::string, Entry> cache;                        ///< 键：host:port
  std::map<std::string, std::shared_ptr<Pending>> inflight;  ///< 正在进行的解析
};

namespace {

// 在后台解析并更新缓存。参数都按值传递：调用者可能早已不在了
Task<> refresh(std::weak_ptr<DnsCache::State> weak, std::shared_ptr<Pending> pending,
               Resolver resolver, std::string key, std::string host, std::uint16_t port) {
  Result<Endpoints> r = fail(ErrorCode::kUpstreamUnavailable, "DNS 解析出错");
  try {
    r = co_await resolver(std::move(host), port);
  } catch (const std::exception& e) {
    r = fail(ErrorCode::kUpstreamUnavailable, std::string("DNS 解析出错：") + e.what());
  }
  if (r && r->empty()) r = fail(ErrorCode::kUpstreamUnavailable, "DNS 没有返回任何地址");
  if (auto st = weak.lock()) {
    st->inflight.erase(key);
    const auto now = SteadyClock::now();
    auto it = st->cache.find(key);
    if (r) {
      st->cache[key] = Entry{*r, {}, now + st->ttl};
    } else if (it != st->cache.end() && !it->second.endpoints.empty()) {
      // 重新解析失败：继续用旧结果，再保留一个 TTL
      it->second.expires = now + st->ttl;
      r = it->second.endpoints;
    } else {
      st->cache[key] = Entry{{}, r.error(), now + kNegativeTtl};
    }
  }
  pending->result = std::move(r);
  pending->done.cancel();
}

}  // namespace

DnsCache::DnsCache(asio::any_io_executor executor, Resolver resolver, SteadyClock::duration ttl)
    : executor_(std::move(executor)), state_(std::make_shared<State>()) {
  state_->resolver = resolver ? std::move(resolver) : asio_resolver();
  state_->ttl = ttl;
}

Task<Result<Endpoints>> DnsCache::resolve(std::string host, std::uint16_t port) {
  boost::system::error_code ec;
  const auto address = asio::ip::make_address(host, ec);
  if (!ec) co_return Endpoints{tcp::endpoint(address, port)};

  const std::shared_ptr<State> st = state_;
  std::string key = host + ":" + std::to_string(port);
  std::optional<Endpoints> old;  // 过期的旧结果（复制出来：后台刷新会修改缓存）
  if (auto it = st->cache.find(key); it != st->cache.end()) {
    const bool positive = !it->second.endpoints.empty();
    if (SteadyClock::now() < it->second.expires) {
      if (positive) co_return it->second.endpoints;
      co_return fail(it->second.error);  // 1 秒内刚失败过
    }
    if (positive) old = it->second.endpoints;
  }

  std::shared_ptr<Pending> pending;
  if (auto f = st->inflight.find(key); f != st->inflight.end()) {
    pending = f->second;
  } else {
    pending = std::make_shared<Pending>(executor_);
    st->inflight.emplace(key, pending);
    asio::co_spawn(executor_, refresh(st, pending, st->resolver, key, std::move(host), port),
                   asio::detached);
  }
  if (old.has_value()) co_return std::move(*old);  // 过期了：先用旧结果，刷新在后台进行

  // 解析可能已经同步完成（取消发生在等待之前不会唤醒等待者），所以先检查
  if (!pending->result.has_value())
    co_await pending->done.async_wait(asio::as_tuple(asio::use_awaitable));
  const std::optional<Result<Endpoints>>& got = pending->result;
  if (!got.has_value()) {
    // 被取消（连接超时、客户端断开）：后台的解析继续进行，结束后更新缓存
    co_return fail(ErrorCode::kUpstreamUnavailable, "DNS 解析被取消");
  }
  co_return *got;
}

Resolver asio_resolver() {
  // 不捕获任何东西：协程在 lambda 返回之后才执行完，捕获的变量可能已经不在了
  return [](std::string host, std::uint16_t port) -> Task<Result<Endpoints>> {
    tcp::resolver resolver(co_await asio::this_coro::executor);
    auto [ec, results] = co_await resolver.async_resolve(host, std::to_string(port),
                                                         asio::as_tuple(asio::use_awaitable));
    if (ec) co_return fail(ErrorCode::kUpstreamUnavailable, "DNS 解析失败：" + ec.message());
    Endpoints out;
    for (const auto& r : results) out.push_back(r.endpoint());
    co_return out;
  };
}

}  // namespace llmgw::upstream::detail
