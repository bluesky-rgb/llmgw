#pragma once

#include <memory>
#include <string>

#include "config.h"
#include "metrics.h"

namespace llmgw {

// 网关服务本体。用 pimpl 把 httplib 挡在头文件之外，
// 以后把底层换成 Drogon / Boost.Beast 时，调用方不需要改动
class Gateway {
 public:
  explicit Gateway(Config cfg);
  ~Gateway();
  Gateway(const Gateway&) = delete;
  Gateway& operator=(const Gateway&) = delete;

  // port 为 0 时随机选一个空闲端口（测试用）。成功返回实际端口，失败返回 -1
  int bind(const std::string& host, int port);
  // 阻塞运行，直到 stop() 被调用
  bool run();
  void stop();
  void wait_until_ready() const;

  Metrics& metrics();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace llmgw
