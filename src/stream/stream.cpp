// SseStream（按完整的事件切块）、BodyStream（原样转发），以及 open()。
#include "llmgw/stream/stream.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <deque>
#include <utility>

#include "inspect.h"
#include "llmgw/stream/sse_parser.h"

namespace llmgw::stream {

namespace {

bool iequals_prefix(std::string_view s, std::string_view prefix) {
  return s.size() >= prefix.size() &&
         std::equal(prefix.begin(), prefix.end(), s.begin(), [](char a, char b) {
           return std::tolower(static_cast<unsigned char>(a)) ==
                  std::tolower(static_cast<unsigned char>(b));
         });
}

// 媒体类型恰好是 text/event-stream（后面只能是结尾、参数或空白；text/event-stream-foo 不算）
bool is_event_stream_type(std::string_view v) {
  while (!v.empty() && (v.front() == ' ' || v.front() == '\t')) v.remove_prefix(1);
  constexpr std::string_view kType = "text/event-stream";
  if (!iequals_prefix(v, kType)) return false;
  return v.size() == kType.size() || v[kType.size()] == ';' || v[kType.size()] == ' ' ||
         v[kType.size()] == '\t';
}

// 2xx，并且所有的 Content-Type 都是 text/event-stream（不一致时按原样转发处理，独立审查 #5）
bool is_event_stream(const ResponseHead& head) {
  if (head.status < 200 || head.status > 299) return false;
  bool found = false;
  for (const auto& h : head.headers) {
    if (h.name.size() != 12 || !iequals_prefix(h.name, "content-type")) continue;
    if (!is_event_stream_type(h.value)) return false;
    found = true;
  }
  return found;
}

bool mentions_usage(std::string_view data) {
  // 先筛：大多数事件不需要解析 JSON
  return memmem(data.data(), data.size(), "\"usage\"", 7) != nullptr;
}

bool is_done_line(std::string_view line) { return line == "data: [DONE]" || line == "data:[DONE]"; }

// 预读时每个事件按原始字节数加上这么多计算（SseEvent
// 对象本身的开销）：只发空行的上游会产生大量很小的事件
constexpr std::size_t kHeldEventOverhead = 128;

// ---------- SSE ----------

class SseStream final : public ResponseStream {
 public:
  SseStream(ResponseHead head, std::unique_ptr<Source> source, RequestContext* ctx, Options options)
      : head_(std::move(head)),
        source_(std::move(source)),
        ctx_(ctx),
        options_(options),
        parser_(options.max_event_bytes) {}

  [[nodiscard]] const ResponseHead& head() const override { return head_; }
  ResponseHead* mutable_head() { return &head_; }

  Task<Result<std::optional<Chunk>>> next() override {
    for (;;) {
      std::string out;
      while (auto ev = take()) {
        if (inspect(*ev)) out += ev->raw;
      }
      if (finished_) out += std::exchange(passthrough_, {});
      // 先交出已经完整的事件，再报告错误：否则结果取决于切块方式（独立审查 #3）
      if (!out.empty()) co_return Chunk{std::move(out)};
      if (deferred_.has_value()) {
        Error error = *deferred_;
        deferred_.reset();
        co_return fail(std::move(error));
      }
      if (finished_) co_return std::optional<Chunk>{};
      co_await pull();
    }
  }

  /// 预读到第一个带 data 的事件（open() 检查业务错误码时使用），返回要检查的 JSON：
  /// 第一个带 data 的事件的 data；整个响应体里没有任何 data 事件时（比如一个 JSON
  /// 错误体），返回整个响应体； 预读超过上限时返回空（放弃检查，之后的 next()
  /// 照常返回预读的数据）。 读取出错时返回错误：此时还没有向客户端发送任何东西，executor
  /// 可以安全地换一个上游
  Task<Result<std::optional<std::string>>> prefetch_for_check() {
    std::size_t held_bytes = 0;
    for (;;) {
      while (auto ev = parser_.next()) {
        held_bytes += ev->raw.size() + kHeldEventOverhead;
        if (ev->has_data) {
          std::optional<std::string> data = ev->data;
          held_.push_back(std::move(*ev));
          co_return data;
        }
        held_.push_back(std::move(*ev));
      }
      if (read_failed_ && deferred_.has_value()) co_return fail(*deferred_);
      if (finished_ && !deferred_) {
        std::optional<std::string> whole{std::string()};
        for (const auto& e : held_) *whole += e.raw;
        *whole += passthrough_;
        co_return whole;
      }
      if (deferred_ || held_bytes > options_.max_event_bytes) {
        // 只发注释和空行、迟迟不发 data 的上游：不能无限预读（独立审查 #1）
        std::fprintf(stderr, "[stream] request %s: gave up checking the business error code\n",
                     ctx_->request.request_id.c_str());
        co_return std::optional<std::string>{};
      }
      co_await pull();
    }
  }

 private:
  std::optional<SseEvent> take() {
    if (!held_.empty()) {
      SseEvent ev = std::move(held_.front());
      held_.pop_front();
      return ev;
    }
    return parser_.next();
  }

  // 从来源读一段喂给解析器；来源结束时收尾。出错时记在 deferred_ 中
  Task<> pull() {
    if (source_done_) {
      end_of_body();
      co_return;
    }
    in_.clear();
    auto r = co_await source_->read(&in_);
    if (!r) {
      deferred_ = r.error();
      read_failed_ = true;
      finished_ = true;
      co_return;
    }
    if (!*r) {
      source_done_ = true;
      co_return;
    }
    parser_.feed(in_);
    if (parser_.overflowed()) {
      deferred_ = Error{
          .code = ErrorCode::kUpstreamUnavailable,
          .message = "上游的 SSE 事件超过 " + std::to_string(options_.max_event_bytes) + " 字节",
          .retry_after = {}};
      finished_ = true;
    }
  }

  void end_of_body() {
    finished_ = true;
    // 最后一行是没有换行的 "data: [DONE]"：补全后转发，客户端能收到 [DONE]（独立审查 #8）
    if (is_done_line(parser_.unterminated_line())) parser_.feed("\n");
    if (parser_.finish()) return;
    if (parser_.overflowed()) {
      deferred_ = Error{
          .code = ErrorCode::kUpstreamUnavailable,
          .message = "上游的 SSE 事件超过 " + std::to_string(options_.max_event_bytes) + " 字节",
          .retry_after = {}};
      return;
    }
    if (parser_.dispatched() == 0) {
      // 整个响应体里没有任何 SSE 事件（比如 200 + text/event-stream 的 JSON
      // 错误体）：原样转发（独立审查 #9）
      passthrough_ = std::string(parser_.pending_raw());
      return;
    }
    if (saw_done_ || parser_.unterminated_line().starts_with(':'))
      return;  // 丢弃：与 WHATWG 规范相同
    deferred_ = Error{.code = ErrorCode::kUpstreamUnavailable,
                      .message = "上游的流在一个事件的中间结束",
                      .retry_after = {}};
  }

  // 检查一个事件；返回是否转发
  bool inspect(const SseEvent& ev) {
    if (!ev.has_data) return true;  // 注释、心跳：原样转发，不算首字
    if (!ctx_->ttft) ctx_->ttft = SteadyClock::now() - ctx_->request.received_at;
    if (ev.data == "[DONE]") {
      saw_done_ = true;
      return true;
    }
    if (!mentions_usage(ev.data)) return true;
    auto found = detail::extract_usage(ev.data);
    if (!found) return true;
    ctx_->usage = found->usage;  // 取最后一个非空的
    return !(options_.strip_usage_chunk && found->usage_only);
  }

  ResponseHead head_;
  std::unique_ptr<Source> source_;
  RequestContext* ctx_;
  Options options_;
  SseParser parser_;
  std::deque<SseEvent> held_;      ///< 预读时取出、还没有转发的事件
  std::string passthrough_;        ///< 不是 SSE 的响应体：结束时原样转发
  std::optional<Error> deferred_;  ///< 交出已经完整的事件之后再报告的错误
  std::string in_;
  bool source_done_ = false;
  bool read_failed_ = false;
  bool finished_ = false;
  bool saw_done_ = false;
};

// ---------- 非 SSE：原样转发 ----------

class BodyStream final : public ResponseStream {
 public:
  BodyStream(ResponseHead head, std::unique_ptr<Source> source, RequestContext* ctx,
             Options options)
      : head_(std::move(head)), source_(std::move(source)), ctx_(ctx), options_(options) {}

  [[nodiscard]] const ResponseHead& head() const override { return head_; }
  ResponseHead* mutable_head() { return &head_; }

  Task<Result<std::optional<Chunk>>> next() override {
    if (!held_.empty()) co_return Chunk{std::exchange(held_, {})};
    for (;;) {
      if (done_) {
        // usage 在响应被读完时才写入：被映射成错误、没有被读取的响应不写（独立审查 #4）
        finish();
        co_return std::optional<Chunk>{};
      }
      std::string piece;
      auto r = co_await source_->read(&piece);
      if (!r) co_return fail(r.error());
      if (!*r) {
        done_ = true;
        continue;
      }
      if (piece.empty()) continue;
      keep(piece);
      co_return Chunk{std::move(piece)};
    }
  }

  /// 预读整个响应体（不超过 max_inspect_bytes）。返回是否完整读到了
  Task<Result<bool>> prefetch_all() {
    while (!done_ && held_.size() <= options_.max_inspect_bytes) {
      const std::size_t before = held_.size();
      auto r = co_await source_->read(&held_);
      if (!r) co_return fail(r.error());
      if (!*r) {
        done_ = true;
        break;
      }
      keep(std::string_view(held_).substr(before));
    }
    if (!done_) {
      std::fprintf(
          stderr,
          "[stream] request %s: body larger than %zu bytes, business error code not checked\n",
          ctx_->request.request_id.c_str(), options_.max_inspect_bytes);
    }
    co_return done_;
  }

  [[nodiscard]] const std::string& copy() const { return copy_; }

 private:
  void keep(std::string_view piece) {
    if (copy_dropped_) return;
    if (copy_.size() + piece.size() > options_.max_inspect_bytes) {
      copy_dropped_ = true;  // 太大：放弃副本（只是拿不到 usage）
      copy_ = std::string();
      return;
    }
    copy_.append(piece);
  }
  void finish() {
    if (finished_ || copy_dropped_) return;
    finished_ = true;
    if (!mentions_usage(copy_)) return;
    if (auto found = detail::extract_usage(copy_)) ctx_->usage = found->usage;
  }

  ResponseHead head_;
  std::unique_ptr<Source> source_;
  RequestContext* ctx_;
  Options options_;
  std::string held_;  ///< 预读的数据，第一次 next() 时返回
  std::string copy_;
  bool copy_dropped_ = false;
  bool done_ = false;
  bool finished_ = false;
};

}  // namespace

Task<Result<std::unique_ptr<ResponseStream>>> open(ResponseHead head,
                                                   std::unique_ptr<Source> source,
                                                   RequestContext* ctx, Options options) {
  const bool check = options.error_mapping != nullptr && head.status >= 200 && head.status <= 299;
  if (is_event_stream(head)) {
    auto s = std::make_unique<SseStream>(std::move(head), std::move(source), ctx, options);
    if (check) {
      auto checked = co_await s->prefetch_for_check();
      if (!checked) co_return fail(checked.error());
      const std::optional<std::string>& json = *checked;
      if (json.has_value()) {
        if (auto status = detail::mapped_status(*json, *options.error_mapping))
          s->mutable_head()->status = *status;
      }
    }
    co_return std::unique_ptr<ResponseStream>(std::move(s));
  }
  auto b = std::make_unique<BodyStream>(std::move(head), std::move(source), ctx, options);
  if (check) {
    auto complete = co_await b->prefetch_all();
    if (!complete) co_return fail(complete.error());
    if (*complete) {
      if (auto status = detail::mapped_status(b->copy(), *options.error_mapping))
        b->mutable_head()->status = *status;
    }
  }
  co_return std::unique_ptr<ResponseStream>(std::move(b));
}

}  // namespace llmgw::stream
