// 内置插件 auth（任务 2.2）。
//
// on_request：Bearer token → SHA-256 → 查表（命中后再用 CRYPTO_memcmp 比较 32 字节）。
//   时序：查表的耗时只取决于 SHA-256(token)，攻击者无法由此逼近任何一个 key
//   的原文（要找到哈希值相近的 token 等于攻破 SHA-256）；CRYPTO_memcmp
//   只是多一道保险，不是安全性的来源（独立审查 2.2）。
//   - 找到：写 ctx->auth.key_id，放行
//   - 没找到 / 没有 token：记一次失败；这个地址 1 分钟内失败太多次时返回 429，否则 401
//   先校验、后限速：有效的 key 永远不会因为同一个出口地址后面别人的失败而被拒绝
// token 是密钥：这里不记录、不回显它，日志中只出现客户端地址和 key_id。
#include "llmgw/middleware/auth.h"

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

#include <array>
#include <boost/asio/ip/address.hpp>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace llmgw::middleware {
namespace {

using Digest = std::array<unsigned char, 32>;
constexpr std::string_view kHashPrefix = "sha256:";
constexpr std::string_view kKeyPrefix = "sk-gw-";
constexpr auto kWindow = std::chrono::seconds(60);
constexpr std::size_t kMaxTrackedAddresses = 4096;
constexpr std::int64_t kDefaultFailuresPerMinute = 20;

/// 预先取得的 SHA-256 实现。OpenSSL 3 中 EVP_sha256()
/// 是“隐式获取”：每次调用都要按名字查一次算法表， 第一次调用还要加载算法提供者（实测约 1.9
/// MB，任务 2.2 A7）。插件创建时取一次，请求中直接使用； EVP_MD 是只读的，可以在多个线程上同时用。
using Md = std::unique_ptr<EVP_MD, decltype(&EVP_MD_free)>;
Md fetch_sha256() { return {EVP_MD_fetch(nullptr, "SHA256", nullptr), &EVP_MD_free}; }

Digest sha256(std::string_view data, const EVP_MD* md) {
  Digest out{};
  unsigned int len = 0;
  // EVP_Digest 是一次性的接口，内部不保存状态，可以在多个线程上同时调用
  if (md == nullptr || EVP_Digest(data.data(), data.size(), out.data(), &len, md, nullptr) != 1 ||
      len != out.size()) {
    out.fill(0);  // 不会发生（SHA-256 总是可用）；全零的哈希不会与任何配置的哈希相等
  }
  return out;
}

std::string to_hex(const Digest& d) {
  static constexpr std::string_view kHex = "0123456789abcdef";
  std::string s;
  s.reserve(d.size() * 2);
  for (const unsigned char b : d) {
    s.push_back(kHex[b >> 4U]);
    s.push_back(kHex[b & 0xFU]);
  }
  return s;
}

// "sha256:<hex>"（配置加载时已经校验过格式、转成了小写）→ 32 字节
Digest parse_hash(std::string_view hash) {
  Digest d{};
  if (!hash.starts_with(kHashPrefix))
    return d;  // 不会发生（配置已经校验）；全零不会与任何 token 的哈希相等
  hash.remove_prefix(kHashPrefix.size());
  auto nibble = [](char c) {
    return static_cast<unsigned char>(c <= '9' ? c - '0' : c - 'a' + 10);
  };
  for (std::size_t i = 0; i < d.size() && 2 * i + 1 < hash.size(); ++i) {
    d[i] = static_cast<unsigned char>((nibble(hash[2 * i]) << 4U) | nibble(hash[2 * i + 1]));
  }
  return d;
}

struct DigestHash {
  std::size_t operator()(const Digest& d) const noexcept {
    std::size_t h = 0;  // SHA-256 的输出是均匀的：取前 8 个字节就是一个好的哈希值
    std::memcpy(&h, d.data(), sizeof(h));
    return h;
  }
};

/// 限速的计数单位：IPv4 按地址；IPv6 按 /64（一个用户通常有整个 /64，按地址计数等于没有限速）。
/// IPv4 映射的 IPv6 地址（::ffff:a.b.c.d，双栈监听时的 IPv4 客户端）按 IPv4 地址计数：
/// 按 /64 截断会让所有 IPv4 客户端落进同一个桶（独立审查 2.2 #3）。Session
/// 已经做了转换，这里再做一次
std::string limiter_key(const std::string& address) {
  boost::system::error_code ec;
  const auto addr = boost::asio::ip::make_address(address, ec);
  if (ec || !addr.is_v6()) return address;
  if (addr.to_v6().is_v4_mapped()) {
    return boost::asio::ip::make_address_v4(boost::asio::ip::v4_mapped, addr.to_v6()).to_string();
  }
  auto bytes = addr.to_v6().to_bytes();
  std::fill(bytes.begin() + 8, bytes.end(), 0);
  return boost::asio::ip::address_v6(bytes).to_string() + "/64";
}

/// 按地址统计 1 分钟窗口内的鉴权失败次数。每个 Worker 一份，不加锁。
class FailureTracker {
 public:
  explicit FailureTracker(std::int64_t limit) : limit_(limit) {}

  struct Verdict {
    bool throttled = false;
    bool first_in_window = false;  ///< 这个地址在本窗口内的第一次失败（记一行日志）
    bool just_throttled = false;  ///< 刚好在这一次超过阈值（记一行日志）
    bool untracked = false;       ///< 地址表满了，没有记录这个地址
    std::chrono::seconds retry_after{0};
  };

  Verdict record(const std::string& key, SteadyClock::time_point now) {
    auto it = entries_.find(key);
    if (it == entries_.end()) {
      if (entries_.size() >= kMaxTrackedAddresses) sweep(now);
      // 仍然满了：不再记录新的地址。这只是不限速，鉴权本身照常进行（失败仍然是 401）
      if (entries_.size() >= kMaxTrackedAddresses) {
        Verdict v;
        v.untracked = true;
        return v;
      }
      it = entries_.emplace(key, Entry{.window_start = now, .count = 0}).first;
    } else if (now - it->second.window_start >= kWindow) {
      it->second = Entry{.window_start = now, .count = 0};
    }
    const std::int64_t count = ++it->second.count;
    const auto left = it->second.window_start + kWindow - now;
    return Verdict{.throttled = count > limit_,
                   .first_in_window = count == 1,
                   .just_throttled = count == limit_ + 1,
                   .retry_after = std::max(std::chrono::seconds(1),
                                           std::chrono::ceil<std::chrono::seconds>(left))};
  }

  [[nodiscard]] std::size_t size() const { return entries_.size(); }

 private:
  struct Entry {
    SteadyClock::time_point window_start;
    std::int64_t count = 0;
  };
  // 清掉窗口已经过期的地址。最多每秒一次：地址表被大量不同的地址填满时，不能每次失败都扫一遍
  void sweep(SteadyClock::time_point now) {
    if (now - last_sweep_ < std::chrono::seconds(1)) return;
    last_sweep_ = now;
    std::erase_if(entries_, [&](const auto& e) { return now - e.second.window_start >= kWindow; });
  }

  std::int64_t limit_;
  std::unordered_map<std::string, Entry> entries_;
  SteadyClock::time_point last_sweep_;
};

Error error_of(ErrorCode code, std::string message, std::chrono::seconds retry_after = {}) {
  return Error{.code = code,
               .message = std::move(message),
               .retry_after = retry_after.count() > 0
                                  ? std::optional<std::chrono::seconds>(retry_after)
                                  : std::nullopt};
}

class Auth final : public Middleware {
 public:
  Auth(const config::Snapshot& snapshot, std::int64_t failures_per_minute, MetricsRecorder* metrics)
      : failures_(failures_per_minute), metrics_(metrics) {
    for (const config::KeyConfig& k : snapshot.keys) by_hash_.emplace(parse_hash(k.hash), k.id);
  }

  Task<Decision> on_request(RequestContext* ctx) override {
    const std::string& token = ctx->request.bearer_token;
    if (!token.empty()) {
      const Digest digest = sha256(token, md_.get());
      if (auto it = by_hash_.find(digest);
          it != by_hash_.end() &&
          CRYPTO_memcmp(it->first.data(), digest.data(), digest.size()) == 0) {
        ctx->auth.key_id = it->second;
        co_return Decision::pass();
      }
    }
    co_return reject(*ctx, token.empty());
  }

  Task<> on_complete(const RequestContext* /*ctx*/) override { co_return; }

 private:
  Decision reject(const RequestContext& ctx, bool missing) {
    const std::string& address = ctx.request.client_address;
    const auto v = failures_.record(limiter_key(address), ctx.request.received_at);
    if (v.untracked) {
      // 地址表被大量不同的地址占满（比如来自很多个 /64）：逐个地址的日志没有了，每分钟汇总一行，
      // 不能让监控变成瞎子（独立审查 2.2 #4）
      ++untracked_failures_;
      if (ctx.request.received_at - last_untracked_log_ >= kWindow) {
        std::fprintf(stderr,
                     "[auth] failure tracker is full (%zu addresses): %llu failures from untracked "
                     "addresses since the last report; latest from %s\n",
                     kMaxTrackedAddresses, static_cast<unsigned long long>(untracked_failures_),
                     address.empty() ? "(unknown)" : address.c_str());
        last_untracked_log_ = ctx.request.received_at;
        untracked_failures_ = 0;
      }
    } else if (v.first_in_window || v.just_throttled) {  // 每个地址每分钟最多两行，不会被刷屏
      std::fprintf(stderr, "[auth] request %s from %s: %s%s\n", ctx.request.request_id.c_str(),
                   address.empty() ? "(unknown)" : address.c_str(),
                   missing ? "missing gateway key" : "invalid gateway key",
                   v.just_throttled ? "; too many failures, answering 429 for this minute" : "");
    }
    if (v.throttled) {
      metrics_->increment(Counter::kRejected, {"auth_throttled"});
      return Decision::deny(
          error_of(ErrorCode::kRateLimited, "鉴权失败次数过多，请稍后再试", v.retry_after));
    }
    metrics_->increment(Counter::kRejected, {"auth"});
    Decision d = Decision::deny(error_of(
        ErrorCode::kInvalidApiKey,
        missing ? "缺少网关 key：请求头应为 Authorization: Bearer <网关 key>" : "网关 key 无效"));
    d.response_headers.push_back({"WWW-Authenticate", "Bearer"});  // RFC 9110 15.5.2：401 必须带
    return d;
  }

  Md md_ = fetch_sha256();
  std::unordered_map<Digest, std::string, DigestHash> by_hash_;
  FailureTracker failures_;
  std::uint64_t untracked_failures_ = 0;
  SteadyClock::time_point last_untracked_log_;
  MetricsRecorder* metrics_;
};

// 插件配置：{"failures_per_minute": N}，都可以不写。返回 N；有问题时写进 issues
std::int64_t parse_config(const PluginSpec& spec, std::vector<config::Issue>* issues) {
  const auto cfg = nlohmann::json::parse(spec.config_json, nullptr, false);
  if (!cfg.is_object()) {  // config 模块已经保证是对象；这里只是防御
    issues->push_back({.severity = config::Severity::kError,
                       .path = spec.path + ".config",
                       .message = "应该是对象"});
    return kDefaultFailuresPerMinute;
  }
  std::int64_t limit = kDefaultFailuresPerMinute;
  for (const auto& [name, value] : cfg.items()) {
    if (name == "failures_per_minute") {
      if (!value.is_number_integer() || value.get<std::int64_t>() < 1 ||
          value.get<std::int64_t>() > 100000) {
        issues->push_back({.severity = config::Severity::kError,
                           .path = spec.path + ".config.failures_per_minute",
                           .message = "应该是 1–100000 的整数"});
      } else {
        limit = value.get<std::int64_t>();
      }
    } else {
      issues->push_back({.severity = config::Severity::kError,
                         .path = spec.path + ".config." + name,
                         .message = "未知字段（auth 插件只有 failures_per_minute）"});
    }
  }
  return limit;
}

class AuthFactory final : public MiddlewareFactory {
 public:
  [[nodiscard]] std::string_view name() const override { return "auth"; }
  [[nodiscard]] std::string_view description() const override {
    return "校验客户端的网关 key（Authorization: Bearer），配置 keys[] 中只保存哈希";
  }
  [[nodiscard]] std::vector<config::Issue> validate(
      const PluginSpec& spec, const config::Snapshot& snapshot) const override {
    std::vector<config::Issue> issues;
    (void)parse_config(spec, &issues);
    if (snapshot.keys.empty()) {
      issues.push_back({.severity = config::Severity::kError,
                        .path = spec.path,
                        .message = "启用了 auth 但没有配置 keys：所有请求都会被拒绝。"
                                   "用 llmgw keygen --id <名字> 生成 key，把输出的哈希写进 keys"});
    }
    return issues;
  }
  [[nodiscard]] std::unique_ptr<Middleware> create(const PluginSpec& spec,
                                                   const config::Snapshot& snapshot,
                                                   const WorkerResources& worker) const override {
    std::vector<config::Issue> ignored;  // validate 已经检查过
    return std::make_unique<Auth>(snapshot, parse_config(spec, &ignored), worker.metrics);
  }
};

}  // namespace

std::string generate_key() {
  std::array<unsigned char, 32> bytes{};
  if (RAND_bytes(bytes.data(), static_cast<int>(bytes.size())) != 1) return {};
  static constexpr std::string_view kAlphabet =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
  std::string key(kKeyPrefix);
  // base64url，不补 "="：每 3 个字节 4 个字符，32 字节 → 43 个字符
  std::uint32_t buffer = 0;
  int bits = 0;
  for (const unsigned char b : bytes) {
    buffer = (buffer << 8U) | b;
    bits += 8;
    while (bits >= 6) {
      bits -= 6;
      key.push_back(kAlphabet[(buffer >> static_cast<unsigned>(bits)) & 0x3FU]);
    }
  }
  if (bits > 0) key.push_back(kAlphabet[(buffer << static_cast<unsigned>(6 - bits)) & 0x3FU]);
  OPENSSL_cleanse(bytes.data(), bytes.size());
  return key;
}

std::string hash_key(std::string_view key) {
  const Md md = fetch_sha256();
  return std::string(kHashPrefix) + to_hex(sha256(key, md.get()));
}

std::unique_ptr<MiddlewareFactory> make_auth_factory() { return std::make_unique<AuthFactory>(); }

}  // namespace llmgw::middleware
