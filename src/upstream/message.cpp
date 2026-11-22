#include "message.h"

#include <algorithm>
#include <array>
#include <boost/asio/as_tuple.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <cctype>
#include <cstdio>
#include <memory>
#include <tuple>

namespace llmgw::upstream::detail {

namespace http = boost::beast::http;
constexpr auto kTuple = asio::as_tuple(asio::use_awaitable);

namespace {

bool iequals(std::string_view a, std::string_view b) {
  return std::equal(a.begin(), a.end(), b.begin(), b.end(), [](char x, char y) {
    return std::tolower(static_cast<unsigned char>(x)) ==
           std::tolower(static_cast<unsigned char>(y));
  });
}

// 由 Client 设置，或者与连接管理有关的请求头：调用者设置的一律忽略
bool reserved(std::string_view name) {
  static constexpr std::array<std::string_view, 10> kReserved{
      "host",       "content-length",  "transfer-encoding",   "connection",
      "keep-alive", "accept-encoding", "proxy-authorization", "proxy-connection",
      "te",         "upgrade"};
  return std::any_of(kReserved.begin(), kReserved.end(),
                     [&](std::string_view r) { return iequals(name, r); });
}

std::string host_header(const config::Url& url) {
  const bool default_port =
      (url.scheme == "https" && url.port == 443) || (url.scheme == "http" && url.port == 80);
  if (default_port)
    return url.host.find(':') != std::string::npos ? "[" + url.host + "]" : url.host;
  return authority(url.host, url.port);
}

auto failure(Stage stage, bool maybe_sent, std::string detail) {
  return unexpected(AttemptError{.stage = stage,
                                 .maybe_sent = maybe_sent,
                                 .timed_out = false,
                                 .reused_without_response = false,
                                 .detail = std::move(detail)});
}

// ---- 响应体 ----

class BodyImpl final : public Body {
 public:
  BodyImpl(Lease lease, std::unique_ptr<detail::Parser> parser)
      : lease_(std::move(lease)), parser_(std::move(parser)) {}
  BodyImpl(const BodyImpl&) = delete;
  BodyImpl& operator=(const BodyImpl&) = delete;
  BodyImpl(BodyImpl&&) = delete;
  BodyImpl& operator=(BodyImpl&&) = delete;
  ~BodyImpl() override { finish(); }

  Task<Outcome<bool>> read(std::string* out) override {
    for (;;) {
      if (failed_) co_return failure(Stage::kBody, true, "之前已经出错");
      if (parser_->is_done()) {
        finish();
        co_return false;
      }
      Connection* conn = lease_.conn();
      // 读进自己的缓冲区，再把读到的字节追加给调用者。不能先把调用者的缓冲区 resize 出 16 KB：
      // resize 会清零，而一个 SSE 事件只有一百多字节（任务 1.9 压测：callgrind 中 58% 的指令是
      // 这里的 memset）。调用者的缓冲区只在读取成功之后才改动：等待时被取消、抛出异常也不会
      // 留下多余的字节（1.4 独立审查 #13）
      if (!scratch_) scratch_ = std::make_unique_for_overwrite<std::array<char, kChunk>>();
      auto& body = parser_->get().body();
      body.data = scratch_->data();
      body.size = kChunk;
      boost::system::error_code ec;
      try {
        std::tie(ec, std::ignore) = co_await conn->with_stream(
            [&](auto& s) { return http::async_read_some(s, conn->buffer(), *parser_, kTuple); });
      } catch (...) {
        body.data = nullptr;
        body.size = 0;
        failed_ = true;
        throw;
      }
      const std::size_t got = kChunk - body.size;
      out->append(scratch_->data(), got);
      body.data = nullptr;
      body.size = 0;
      if (ec == http::error::need_buffer) ec = {};
      if (ec) {
        failed_ = true;
        lease_ = Lease{};  // 关闭连接，归还名额
        co_return failure(Stage::kBody, true, "读取响应体失败：" + ec.message());
      }
      if (got > 0) co_return true;
    }
  }

 private:
  // 响应完整、上游允许复用、没有多余的字节 → 还回连接池；否则关闭
  void finish() {
    if (!lease_.held()) return;
    const bool reusable = !failed_ && parser_->is_done() && parser_->get().keep_alive() &&
                          !parser_->need_eof() && lease_.conn() != nullptr &&
                          lease_.conn()->buffer().size() == 0;
    if (reusable) {
      lease_.give_back(SteadyClock::now());
    } else {
      lease_ = Lease{};
    }
  }

  // 每次最多读这么多：SSE 的一个事件通常几百字节，非流式响应的大块数据分几次读
  static constexpr std::size_t kChunk = std::size_t{16} * 1024;

  Lease lease_;
  std::unique_ptr<detail::Parser> parser_;
  std::unique_ptr<std::array<char, kChunk>> scratch_;  // 第一次读取时分配，不初始化
  bool failed_ = false;
};

}  // namespace

std::unique_ptr<Body> make_body(Lease lease, std::unique_ptr<Parser> parser) {
  return std::make_unique<BodyImpl>(std::move(lease), std::move(parser));
}

bool valid_header(std::string_view name, std::string_view value) {
  constexpr std::string_view kTokenExtra = "!#$%&'*+-.^_`|~";
  const bool name_ok = !name.empty() && std::all_of(name.begin(), name.end(), [&](char c) {
    return std::isalnum(static_cast<unsigned char>(c)) != 0 ||
           kTokenExtra.find(c) != std::string_view::npos;
  });
  const bool value_ok = std::none_of(value.begin(), value.end(), [](char c) {
    const auto u = static_cast<unsigned char>(c);
    return (u < 0x20 && u != '\t') || u == 0x7F;
  });
  return name_ok && value_ok;
}

namespace {

bool is_token(std::string_view s) {
  constexpr std::string_view kTokenExtra = "!#$%&'*+-.^_`|~";
  return !s.empty() && std::all_of(s.begin(), s.end(), [&](char c) {
    return std::isalnum(static_cast<unsigned char>(c)) != 0 ||
           kTokenExtra.find(c) != std::string_view::npos;
  });
}

// origin-form（RFC 9112 3.2.1）：以 "/" 开头，不含空白、控制字符和非 ASCII
// 字节（应当已经百分号编码）
bool valid_target(std::string_view t) {
  return t.starts_with('/') && std::none_of(t.begin(), t.end(), [](char c) {
           const auto u = static_cast<unsigned char>(c);
           return u <= 0x20 || u >= 0x7F;
         });
}

}  // namespace

Outcome<Message> build_message(const Route& route, Request request) {
  if (!is_token(request.method)) return failure(Stage::kSend, false, "请求方法不合法");
  if (!valid_target(request.target))
    return failure(Stage::kSend, false, "请求目标不合法（必须以 / 开头，不含空白和控制字符）");
  const config::Url& url = route.provider->base_url;
  Message msg;
  const auto verb = http::string_to_verb(request.method);
  if (verb == http::verb::unknown) {
    msg.method_string(request.method);
  } else {
    msg.method(verb);
  }
  // HTTP 上游经过代理：绝对路径形式（RFC 9112 3.2.2），代理据此转发
  msg.target(route.absolute_form() ? "http://" + authority(url.host, url.port) + request.target
                                   : request.target);
  msg.version(11);
  for (auto& h : request.headers) {
    if (reserved(h.name)) continue;
    if (!valid_header(h.name, h.value)) {
      std::fprintf(stderr, "[upstream] %s: dropped invalid request header\n",
                   route.provider->name.c_str());
      continue;
    }
    msg.insert(h.name, h.value);
  }
  msg.set(http::field::host, host_header(url));
  // 不接受压缩：SSE 需要逐块转发，网关也要读取响应内容（用量统计等）
  msg.set(http::field::accept_encoding, "identity");
  if (route.absolute_form() && !route.proxy->username.empty()) {
    msg.set(http::field::proxy_authorization,
            basic_auth(route.proxy->username, route.proxy->password));
  }
  msg.keep_alive(true);
  msg.body() = std::move(request.body);
  msg.prepare_payload();
  return msg;
}

Task<Outcome<std::unique_ptr<Parser>>> read_final_head(Connection* conn, bool head_request) {
  // 一个上游最多先回几个 1xx：防止恶意的上游用无穷的 1xx 让我们一直读下去（虽然 first_byte
  // 也会结束它）
  constexpr int kMaxInformational = 8;
  for (int i = 0; i <= kMaxInformational; ++i) {
    auto parser = std::make_unique<Parser>();
    parser->header_limit(static_cast<std::uint32_t>(kMaxResponseHeaderBytes));
    parser->body_limit(boost::none);  // 响应体不限大小：边读边转发，不在内存中累积
    if (head_request) parser->skip(true);
    const std::size_t buffered = conn->buffer().size();
    auto [ec, n] = co_await conn->with_stream(
        [&](auto& s) { return http::async_read_header(s, conn->buffer(), *parser, kTuple); });
    if (ec) {
      AttemptError e;
      e.stage = Stage::kResponse;
      e.maybe_sent = true;
      e.reused_without_response = i == 0 && buffered == 0 && !parser->got_some() &&
                                  (ec == http::error::end_of_stream || ec == asio::error::eof ||
                                   ec == asio::error::connection_reset);
      e.detail = (ec == http::error::header_limit || ec == http::error::buffer_overflow)
                     ? "上游的响应头超过 64 KB"
                     : "读取响应头失败：" + ec.message();
      co_return unexpected(std::move(e));
    }
    const unsigned status = parser->get().result_int();
    if (status == 101) {
      co_return unexpected(AttemptError{
          .stage = Stage::kResponse, .maybe_sent = true, .detail = "上游要求切换协议（101）"});
    }
    if (status >= 200 || status < 100) {
      if (status < 100 || status > 599) {
        co_return unexpected(
            AttemptError{.stage = Stage::kResponse,
                         .maybe_sent = true,
                         .detail = "上游返回了不合法的状态码 " + std::to_string(status)});
      }
      co_return parser;
    }
    // 1xx（比如 100 Continue）：跳过，继续读最终响应
  }
  co_return unexpected(AttemptError{
      .stage = Stage::kResponse, .maybe_sent = true, .detail = "上游返回了太多 1xx 响应"});
}

}  // namespace llmgw::upstream::detail
