// 内部：各部分的解析函数（实现在 sections.cpp）。
#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "json_reader.h"
#include "llmgw/config/loader.h"
#include "llmgw/config/snapshot.h"

namespace llmgw::config::detail {

/// 目标级的超时覆盖：没写的项沿用全局值。
/// 没有 total：总超时覆盖整个请求（包括切换到其他目标），按目标覆盖没有意义。
struct TimeoutOverrides {
  std::optional<Seconds> connect;
  std::optional<Seconds> first_byte;
  std::optional<Seconds> first_byte_non_stream;
  std::optional<Seconds> idle;
};

Timeouts merge(const Timeouts& global, const TimeoutOverrides& overrides);

/// connect、first_byte、idle 等不能大于 total。
void check_against_total(const Timeouts& t, const std::string& path, Diagnostics* diag);
/// 同上，但只检查目标自己覆盖的项。
void check_overrides_against_total(const TimeoutOverrides& o, Seconds total,
                                   const std::string& path, Diagnostics* diag);

bool is_loopback(std::string_view host);

// 每个函数读取一个部分，记录问题，最后调用 finish() 报告未知字段。
// 参数 r 按值传入：ObjectReader 很小（几个指针和两个短 vector），读完即丢弃。
ServerConfig read_server(ObjectReader r);
Timeouts read_timeouts(ObjectReader r);
Provider read_provider(ObjectReader r);
ProxyConfig read_proxy(ObjectReader r, const EnvLookup& getenv);
ModelConfig read_model(ObjectReader r, const Timeouts& global);

}  // namespace llmgw::config::detail
