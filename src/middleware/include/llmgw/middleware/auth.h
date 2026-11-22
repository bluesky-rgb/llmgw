// 内置插件 auth：客户端鉴权（任务 2.2）。
//
// 客户端用 `Authorization: Bearer <网关 key>` 访问；配置的 keys[] 中只有 key 的 SHA-256 哈希。
// key 是 256 位随机数（不是人设的口令），所以用快速哈希就够了，不需要 bcrypt / Argon2
// （设计见 docs/tasks/2.2-auth.md 第 6 节）。
#pragma once

#include <memory>
#include <string>
#include <string_view>

#include "llmgw/middleware/middleware.h"

namespace llmgw::middleware {

/// 生成一个新的网关 key："sk-gw-" + 32 字节随机数（OpenSSL RAND_bytes）的 base64url（43 个字符）。
/// 随机数生成失败时返回空字符串。
[[nodiscard]] std::string generate_key();

/// key 的哈希，配置中写的就是它："sha256:" + 64 位小写十六进制。
[[nodiscard]] std::string hash_key(std::string_view key);

/// auth 插件的工厂。插件配置（pipeline 中的 "config"，都可以不写）：
///   failures_per_minute  同一个客户端地址（IPv4 按地址，IPv6 按 /64）每分钟允许的鉴权失败次数，
///                        超过之后失败返回 429 而不是 401；默认 20。有效的 key 永远不受影响
[[nodiscard]] std::unique_ptr<MiddlewareFactory> make_auth_factory();

}  // namespace llmgw::middleware
