// stream 模块测试的公共部分：按脚本吐出数据的来源、在 io_context 上运行协程、读完整个流。
#pragma once

#include <gtest/gtest.h>

#include <boost/asio/as_tuple.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <chrono>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "llmgw/stream/stream.h"

namespace llmgw::stream {

namespace asio = boost::asio;
using namespace std::chrono_literals;

// 按脚本吐出数据的来源
struct Step {
  std::string bytes;
  std::chrono::milliseconds delay{0};  ///< 读之前等待（用于 TTFT、取消）
};
class ScriptSource final : public Source {
 public:
  explicit ScriptSource(std::vector<Step> steps, std::optional<Error> error_at_end = std::nullopt,
                        std::shared_ptr<std::size_t> bytes_read = nullptr)
      : steps_(steps.begin(), steps.end()),
        error_(std::move(error_at_end)),
        bytes_read_(std::move(bytes_read)) {}
  Task<Result<bool>> read(std::string* out) override {
    if (steps_.empty()) {
      if (error_) co_return fail(*error_);
      co_return false;
    }
    Step s = std::move(steps_.front());
    steps_.pop_front();
    if (s.delay.count() > 0) {
      asio::steady_timer t(co_await asio::this_coro::executor, s.delay);
      auto [ec] = co_await t.async_wait(asio::as_tuple(asio::use_awaitable));
      if (ec) co_return fail(ErrorCode::kUpstreamUnavailable, "读取被取消");
    }
    out->append(s.bytes);
    if (bytes_read_) *bytes_read_ += s.bytes.size();
    co_return true;
  }

 private:
  std::deque<Step> steps_;
  std::optional<Error> error_;
  std::shared_ptr<std::size_t> bytes_read_;  ///< 统计读出的字节数（检验预读的上限）
};

inline ResponseHead sse_head(int status = 200) {
  return ResponseHead{.status = status,
                      .headers = {{"Content-Type", "text/event-stream; charset=utf-8"}}};
}
inline ResponseHead json_head(int status = 200) {
  return ResponseHead{.status = status, .headers = {{"content-type", "application/json"}}};
}

template <class T>
T run(Task<T> task) {
  asio::io_context ioc;
  std::optional<T> out;
  asio::co_spawn(ioc, std::move(task), [&](std::exception_ptr e, T v) {
    if (e) std::rethrow_exception(e);
    out.emplace(std::move(v));
  });
  ioc.run();
  return std::move(*out);
}

struct Drained {
  int status = 0;
  std::vector<std::string> chunks;
  std::optional<Error> error;  ///< open() 或 next() 的错误
  [[nodiscard]] std::string all() const {
    std::string s;
    for (const auto& c : chunks) s += c;
    return s;
  }
};

inline Task<Drained> drain(ResponseHead head, std::vector<Step> steps, RequestContext* ctx,
                           Options opt = {}, std::optional<Error> error_at_end = std::nullopt) {
  Drained d;
  auto s = co_await open(std::move(head),
                         std::make_unique<ScriptSource>(std::move(steps), std::move(error_at_end)),
                         ctx, opt);
  if (!s) {
    d.error = s.error();
    co_return d;
  }
  d.status = (*s)->head().status;
  for (;;) {
    auto c = co_await (*s)->next();
    if (!c) {
      d.error = c.error();
      break;
    }
    if (!*c) break;
    EXPECT_FALSE((*c)->data.empty()) << "不能返回空块（Session 会当作结束）";
    d.chunks.push_back((*c)->data);
  }
  co_return d;
}

// RequestContext 不能移动：返回纯右值，由拷贝省略直接构造在调用者处
inline RequestContext make_ctx() {
  RequestInfo info;
  info.stream = true;
  info.received_at = SteadyClock::now();
  return RequestContext(std::move(info), nullptr);
}

inline const ErrorMapping kMiniMaxMapping{.path = {"base_resp", "status_code"},
                                          .codes = {{"1002", 429}, {"1008", 402}}};

/// 只调用 open()（不读取流）：检查 open() 改写的状态码、预读了多少字节、写入了什么
inline Task<Drained> open_only(ResponseHead head, std::vector<Step> steps, RequestContext* ctx,
                               Options opt, std::shared_ptr<std::size_t> bytes_read = nullptr,
                               std::optional<Error> error_at_end = std::nullopt) {
  Drained d;
  auto source = std::make_unique<ScriptSource>(std::move(steps), std::move(error_at_end),
                                               std::move(bytes_read));
  auto s = co_await open(std::move(head), std::move(source), ctx, opt);
  if (!s) {
    d.error = s.error();
    co_return d;
  }
  d.status = (*s)->head().status;
  co_return d;
}

}  // namespace llmgw::stream
