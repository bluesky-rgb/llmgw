// 模糊测试：上游的响应（响应头 + 响应体）。输入是上游发来的原始字节，之后上游关闭连接。
// 不变式：不崩溃；读完或出错之后名额一定归还（busy == 0），空闲连接不超过 1 条。
#include <sys/socket.h>
#include <unistd.h>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <cstdint>
#include <memory>
#include <string>

#include "message.h"
#include "pool.h"

namespace {

namespace asio = boost::asio;
using namespace llmgw::upstream::detail;  // NOLINT(google-build-using-namespace)

llmgw::Task<> consume(Lease lease, bool head_request) {
  auto head = co_await read_final_head(lease.conn(), head_request);
  if (!head) co_return;
  auto body = make_body(std::move(lease), std::move(*head));
  std::string out;
  for (int i = 0; i < 100000; ++i) {
    auto more = co_await body->read(&out);
    if (!more || !*more) break;
    if (out.size() > (1U << 20)) out.clear();
  }
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  if (size > 64 * 1024) return 0;
  // 输入的长度为奇数时按 HEAD 请求处理（响应没有响应体），覆盖两条路径而不改变输入的内容
  const bool head_request = size % 2 == 1;  // 小于 socketpair 的缓冲区：一次写完，不需要另一个线程
  static asio::io_context ioc;
  int fds[2];
  if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) return 0;
  std::size_t written = 0;
  while (written < size) {
    const auto n = ::write(fds[1], data + written, size - written);
    if (n <= 0) break;
    written += static_cast<std::size_t>(n);
  }
  ::shutdown(fds[1], SHUT_WR);  // 上游发完后关闭

  auto pool = std::make_shared<Pool>("fuzz", 1, nullptr);
  {
    auto grant = pool->try_take();
    tcp::socket s(ioc);
    s.assign(tcp::v4(), fds[0]);
    asio::co_spawn(ioc,
                   consume(Lease(pool, std::make_unique<Connection>(std::move(s))), head_request),
                   asio::detached);
    ioc.restart();
    ioc.run();
  }
  ::close(fds[1]);
  if (pool->busy_count() != 0 || pool->idle_count() > 1) __builtin_trap();
  return 0;
}
