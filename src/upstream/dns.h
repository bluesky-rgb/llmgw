// 每个 Worker 一份的 DNS 缓存。
//
// - 缓存过期后立即返回旧结果，同时在后台刷新（重新解析失败时继续用旧结果，design.md 5.5）
// - 同一个主机同时只有一个正在进行的解析，等待者共享它的结果
// - 没有旧结果时解析失败：失败结果记住 1 秒，DNS 故障时不会每个请求都发起一次解析
//
// Asio 的 async_resolve 不支持按操作取消（只能对整个 resolver 调用 cancel()），放进竞速时，
// 连接超时要等 getaddrinfo 返回（可能好几秒）才生效。所以解析放在一个独立的协程里执行，
// 调用者等待的是一个可以被取消的定时器：超时或取消时立即返回，解析在后台结束后更新缓存。
#pragma once

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "llmgw/core/request_context.h"
#include "llmgw/core/result.h"
#include "llmgw/core/task.h"
#include "llmgw/upstream/client.h"

namespace llmgw::upstream::detail {

class DnsCache {
 public:
  DnsCache(boost::asio::any_io_executor executor, Resolver resolver, SteadyClock::duration ttl);

  /// 主机名 → 地址列表。IP 地址直接返回。
  Task<Result<std::vector<boost::asio::ip::tcp::endpoint>>> resolve(std::string host,
                                                                    std::uint16_t port);

  struct State;

 private:
  boost::asio::any_io_executor executor_;
  std::shared_ptr<State>
      state_;  ///< 后台的解析通过 weak_ptr 访问：Client 先销毁时不会访问已经不存在的缓存
};

/// 默认的解析函数：Asio 的 async_resolve（在 Asio 内部的专用线程上调用 getaddrinfo）。
Resolver asio_resolver();

}  // namespace llmgw::upstream::detail
