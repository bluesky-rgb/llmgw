// 用一个假的 ResponseStream 跑通 Session 的拉取循环（任务 0.4 设计第 5 节），
// 证明 Task、Result、optional 组合起来可用：正常结束、中途出错两种情况。
#include <gtest/gtest.h>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <deque>
#include <memory>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "llmgw/core/request_context.h"
#include "llmgw/core/response.h"

namespace llmgw {
namespace {

struct End {};
using Item = std::variant<Chunk, Error, End>;

// 按预先给定的顺序返回数据块、错误或结束。每次 next() 都先让出一次执行权，
// 模拟真实实现中“等待上游数据”的异步行为。
class FakeResponseStream final : public ResponseStream {
 public:
  FakeResponseStream(ResponseHead head, std::deque<Item> items)
      : head_(std::move(head)), items_(std::move(items)) {}

  [[nodiscard]] const ResponseHead& head() const override { return head_; }

  [[nodiscard]] Task<Result<std::optional<Chunk>>> next() override {
    co_await boost::asio::post(co_await boost::asio::this_coro::executor,
                               boost::asio::use_awaitable);
    ++calls_;
    Item item = std::move(items_.front());
    items_.pop_front();
    if (auto* c = std::get_if<Chunk>(&item)) co_return std::optional<Chunk>(std::move(*c));
    if (auto* e = std::get_if<Error>(&item)) co_return fail(std::move(*e));
    co_return std::optional<Chunk>{};
  }

  [[nodiscard]] int calls() const { return calls_; }

 private:
  ResponseHead head_;
  std::deque<Item> items_;
  int calls_ = 0;
};

// Session 拉取循环的最小版本。协程参数不用引用（code-architecture.md 3.4），
// 用指针：调用方 co_await 等它结束，所以指向的对象一定还活着。
Task<> relay(RequestContext* ctx, ResponseStream* resp, std::vector<std::string>* client) {
  client->push_back("HEAD " + std::to_string(resp->head().status));
  for (;;) {
    Result<std::optional<Chunk>> r = co_await resp->next();
    if (!r) {
      ctx->outcome = {.kind = OutcomeKind::kFailed,
                      .http_status = resp->head().status,
                      .error = r.error().code};
      client->push_back("ERROR " + r.error().message);
      co_return;
    }
    if (!r->has_value()) {
      ctx->outcome = {
          .kind = OutcomeKind::kSuccess, .http_status = resp->head().status, .error = {}};
      co_return;
    }
    client->push_back(std::move((*r)->data));  // 写完才拉下一块：背压
  }
}

RequestContext make_ctx() {
  return {RequestInfo{.request_id = "r",
                      .model = "m",
                      .stream = true,
                      .body = {},
                      .received_at = {},
                      .deadline = {}},
          nullptr};
}

void run(RequestContext* ctx, ResponseStream* resp, std::vector<std::string>* client) {
  boost::asio::io_context ioc;
  boost::asio::co_spawn(ioc, relay(ctx, resp, client), boost::asio::detached);
  ioc.run();
}

TEST(ResponseStream, pull_loop_forwards_chunks_until_end) {
  FakeResponseStream resp({.status = 200, .headers = {{"content-type", "text/event-stream"}}},
                          {Chunk{"data: a\n\n"}, Chunk{"data: b\n\n"}, End{}});
  auto ctx = make_ctx();
  std::vector<std::string> client;
  run(&ctx, &resp, &client);

  EXPECT_EQ(client, (std::vector<std::string>{"HEAD 200", "data: a\n\n", "data: b\n\n"}));
  EXPECT_EQ(ctx.outcome.kind, OutcomeKind::kSuccess);
  EXPECT_EQ(ctx.outcome.http_status, 200);
  EXPECT_FALSE(ctx.outcome.error.has_value());
  EXPECT_EQ(resp.calls(), 3);  // 结束之后不再调用 next()
}

TEST(ResponseStream, pull_loop_stops_on_mid_stream_error) {
  FakeResponseStream resp({.status = 200, .headers = {}},
                          {Chunk{"data: a\n\n"},
                           Error{.code = ErrorCode::kUpstreamUnavailable,
                                 .message = "upstream closed",
                                 .retry_after = {}},
                           Chunk{"never sent"}});
  auto ctx = make_ctx();
  std::vector<std::string> client;
  run(&ctx, &resp, &client);

  EXPECT_EQ(client, (std::vector<std::string>{"HEAD 200", "data: a\n\n", "ERROR upstream closed"}));
  EXPECT_EQ(ctx.outcome.kind, OutcomeKind::kFailed);
  EXPECT_EQ(ctx.outcome.error, ErrorCode::kUpstreamUnavailable);
  EXPECT_EQ(resp.calls(), 2);  // 出错之后不再调用 next()
}

TEST(ResponseStream, can_be_owned_through_base_pointer) {
  std::unique_ptr<ResponseStream> resp = std::make_unique<FakeResponseStream>(
      ResponseHead{.status = 404, .headers = {}}, std::deque<Item>{End{}});
  EXPECT_EQ(resp->head().status, 404);
}

}  // namespace
}  // namespace llmgw
