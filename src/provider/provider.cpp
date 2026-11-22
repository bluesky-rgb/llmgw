#include "llmgw/provider/provider.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <exception>
#include <memory>
#include <optional>
#include <string>

#include "llmgw/core/version.h"
#include "llmgw/stream/stream.h"
#include "rewrite.h"

namespace llmgw::provider {

namespace {

bool iequals(std::string_view a, std::string_view b) {
  return std::equal(a.begin(), a.end(), b.begin(), b.end(), [](char x, char y) {
    return std::tolower(static_cast<unsigned char>(x)) ==
           std::tolower(static_cast<unsigned char>(y));
  });
}

// upstream::Body → stream::Source。读取失败时把 upstream 原来的 AttemptError 记在 last_error 中：
// open() 预读失败时 attempt() 原样返回它（独立审查 1.6 #6：重新构造会丢掉 timed_out 和 detail）。
// last_error 是共享的：open() 失败时 Source 已经随之销毁
class BodySource final : public stream::Source {
 public:
  BodySource(std::unique_ptr<upstream::Body> body,
             std::shared_ptr<std::optional<upstream::AttemptError>> last_error)
      : body_(std::move(body)), last_error_(std::move(last_error)) {}
  Task<Result<bool>> read(std::string* out) override {
    auto r = co_await body_->read(out);
    if (!r) {
      *last_error_ = r.error();
      co_return fail(r.error().to_error());
    }
    co_return *r;
  }

 private:
  std::unique_ptr<upstream::Body> body_;
  std::shared_ptr<std::optional<upstream::AttemptError>> last_error_;
};

// 持有 stream 的选项所指向的错误码映射（stream 只保存指针），其余转发给 stream 返回的流
class OwningStream final : public ResponseStream {
 public:
  OwningStream(std::unique_ptr<stream::ErrorMapping> mapping, std::unique_ptr<ResponseStream> inner)
      : mapping_(std::move(mapping)), inner_(std::move(inner)) {}
  [[nodiscard]] const ResponseHead& head() const override { return inner_->head(); }
  Task<Result<std::optional<Chunk>>> next() override { return inner_->next(); }

 private:
  std::unique_ptr<stream::ErrorMapping> mapping_;  ///< 必须比 inner_ 活得久：声明在前，析构在后
  std::unique_ptr<ResponseStream> inner_;
};

std::unique_ptr<stream::ErrorMapping> convert(const std::optional<config::ErrorMapping>& m) {
  if (!m) return nullptr;
  auto out = std::make_unique<stream::ErrorMapping>();
  for (const auto& seg : m->field) out->path.push_back(seg.name);  // 配置加载时已经保证没有 []
  out->codes = m->codes;
  return out;
}

}  // namespace

Result<Prepared> prepare(const RequestInfo& request, const config::Provider& provider,
                         const config::TargetConfig& target) {
  detail::Rules rules{.model = target.model,
                      .include_usage = request.stream,
                      .drops = target.drop_fields,
                      .renames = target.rename_fields,
                      .sets = target.set_fields};
  auto rewritten = detail::rewrite(request.body, rules);
  if (!rewritten) return fail(rewritten.error());
  Prepared p;
  p.stream = request.stream;
  p.strip_usage_chunk = rewritten->injected_include_usage;
  p.request.method = "POST";
  p.request.target = provider.base_url.base_path + "/chat/completions";
  p.request.headers = {
      {"Authorization", "Bearer " + provider.api_key},
      {"Content-Type", "application/json"},
      {"Accept", request.stream ? "text/event-stream" : "application/json"},
      {"User-Agent", std::string("llmgw/") + std::string(core::kVersion)},
  };
  p.request.body = std::move(rewritten->body);
  return p;
}

std::vector<Header> filter_response_headers(const std::vector<Header>& upstream_headers) {
  static constexpr std::array<std::string_view, 4> kKeep{"content-type", "cache-control",
                                                         "retry-after", "retry-after-ms"};
  std::vector<Header> out;
  bool request_id = false;
  for (const auto& h : upstream_headers) {
    if (std::any_of(kKeep.begin(), kKeep.end(),
                    [&](std::string_view k) { return iequals(h.name, k); })) {
      out.push_back(h);
    } else if ((iequals(h.name, "x-request-id") || iequals(h.name, "request-id")) && !request_id) {
      out.push_back({"X-LLMGW-Upstream-Request-Id", h.value});  // 两个都有时只转发第一个
      request_id = true;
    }
  }
  return out;
}

Task<Expected<std::unique_ptr<ResponseStream>, upstream::AttemptError>> attempt(
    upstream::Client* client, const config::Provider* provider, Prepared prepared,
    upstream::Deadlines deadlines, RequestContext* ctx) {
  const bool strip = prepared.strip_usage_chunk;
  auto sent = co_await client->send(provider, std::move(prepared.request), deadlines);
  if (!sent) co_return unexpected(sent.error());
  try {
    ResponseHead head{.status = sent->status, .headers = filter_response_headers(sent->headers)};
    auto mapping = convert(provider->error_mapping);
    stream::Options options{.strip_usage_chunk = strip, .error_mapping = mapping.get()};
    auto last_error = std::make_shared<std::optional<upstream::AttemptError>>();
    auto opened = co_await stream::open(
        std::move(head), std::make_unique<BodySource>(std::move(sent->body), last_error), ctx,
        options);
    if (!opened) {
      // 预读（检查业务错误码）时出错：请求已经发出，可能已经计费
      if (*last_error) {
        upstream::AttemptError e = std::move(**last_error);
        e.maybe_sent = true;
        co_return unexpected(std::move(e));
      }
      co_return unexpected(
          upstream::AttemptError{.stage = upstream::Stage::kBody,
                                 .maybe_sent = true,
                                 .timed_out = false,
                                 .reused_without_response = false,
                                 .detail = "读取响应体失败：" + opened.error().message});
    }
    co_return std::unique_ptr<ResponseStream>(
        std::make_unique<OwningStream>(std::move(mapping), std::move(*opened)));
  } catch (const std::exception& e) {
    // 请求已经发出：按“可能已发送”处理
    co_return unexpected(upstream::AttemptError{.stage = upstream::Stage::kBody,
                                                .maybe_sent = true,
                                                .timed_out = false,
                                                .reused_without_response = false,
                                                .detail = std::string("内部错误：") + e.what()});
  }
}

}  // namespace llmgw::provider
