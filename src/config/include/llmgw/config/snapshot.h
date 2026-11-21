// 配置快照：加载后不可变，通过 std::shared_ptr<const Snapshot> 在所有 Worker 之间共享。
//
// 这里是 core 中前置声明的 llmgw::config::Snapshot 的定义（任务 0.4 决策 C）。
// 字段与 docs/design.md 第 7 节一致；尚未支持的部分（keys、pricing 等）见任务 1.1 设计。
#pragma once

#include <chrono>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace llmgw::config {

using Seconds = std::chrono::seconds;

/// 监听地址，比如 "127.0.0.1:9090"。
struct Endpoint {
  std::string host;
  std::uint16_t port = 0;

  friend bool operator==(const Endpoint&, const Endpoint&) = default;
};

/// http(s) URL，比如 "https://api.openai.com/v1"。
struct Url {
  std::string scheme;      ///< "http" 或 "https"
  std::string host;        ///< 主机名或 IP（IPv6 不带方括号）
  std::uint16_t port = 0;  ///< 没有写端口时：https 为 443，http 为 80
  std::string base_path;   ///< 比如 "/v1"；不带结尾的 "/"，没有路径时为空

  friend bool operator==(const Url&, const Url&) = default;
};

/// 三个平面的监听配置（design.md 2.3）。
struct ServerConfig {
  std::string host = "0.0.0.0";  ///< 数据面
  std::uint16_t port = 8080;
  Endpoint admin{"127.0.0.1", 9090};        ///< 管理面：默认只监听本机
  Endpoint observability{"0.0.0.0", 9100};  ///< 观测面：/metrics、/health
  unsigned workers = 0;                     ///< 0 表示等于 CPU 核数
  std::uint32_t max_connections = 10000;    ///< 入站连接总数上限，超出后返回 503
  std::uint64_t max_body_bytes = 32ULL << 20;  ///< 请求体上限（允许带 base64 图片），超出返回 413
  /// 防慢速攻击：读完请求头的最长时间；读请求体时，连续这么久收不到数据就断开
  Seconds read_timeout{10};
  Seconds keep_alive_timeout{60};  ///< 长连接上两个请求之间的最长空闲时间
  /// 写一次响应（一个数据块）给客户端的最长时间：客户端不读时，超过这个时间断开，释放连接名额
  Seconds write_timeout{60};
};

/// 超时（design.md 6.3）。都是正整数秒。
struct Timeouts {
  Seconds connect{10};                 ///< 建立到上游的连接（包括 TLS 握手）
  Seconds first_byte{60};              ///< 流式请求：发出请求后等待响应头
  Seconds first_byte_non_stream{300};  ///< 非流式请求：要等整个回答生成完才有响应头
  Seconds idle{60};                    ///< 流式输出中，两块数据之间的最长间隔
  Seconds total{600};  ///< 整个请求的截止时间，包括所有重试和切换

  friend bool operator==(const Timeouts&, const Timeouts&) = default;
};

/// 字段路径的一段。"messages[].reasoning_content" 解析为
/// {name="messages", each=true}, {name="reasoning_content", each=false}。
struct PathSegment {
  std::string name;
  bool each = false;  ///< 带 "[]"：对数组中的每个元素生效

  friend bool operator==(const PathSegment&, const PathSegment&) = default;
};
using FieldPath = std::vector<PathSegment>;

/// 把响应体中的业务错误码映射成 HTTP 状态码（providers.md 3.4）。
struct ErrorMapping {
  FieldPath field;                   ///< 比如 base_resp.status_code
  std::map<std::string, int> codes;  ///< 业务错误码（文本形式）→ HTTP 状态码
};

/// 一个上游厂商。
struct Provider {
  std::string name;  ///< 只包含字母、数字和 "-_"，会用作指标标签
  Url base_url;
  std::string api_key_env;  ///< 存放密钥的环境变量名
  /// 加载时从 api_key_env 读取的密钥。⚠️ 不能写入日志、指标或任何输出。
  /// 加载时跳过了环境变量（llmgw check --skip-env）则为空。
  std::string api_key;
  std::uint32_t max_connections = 256;  ///< 每个 Worker 到这个上游的最大连接数
  std::optional<ErrorMapping> error_mapping;
};

/// 出站代理。
struct ProxyConfig {
  std::optional<Url> https;  ///< 所有上游默认使用的代理；没有值表示不用代理
  std::map<std::string, Url> per_provider;  ///< 按上游名覆盖
};

/// 字段改名：from → to。
struct FieldRename {
  FieldPath from;
  FieldPath to;
};

/// 强制设置字段：path = value。值保存为 JSON 文本，由 provider 模块解析。
struct FieldSet {
  FieldPath path;
  std::string json_value;
};

/// 模型的一个路由目标。
struct TargetConfig {
  std::string provider;  ///< 引用 providers[].name（加载时已校验存在）
  std::string model;     ///< 发给上游的模型名
  std::vector<FieldPath> drop_fields;
  std::vector<FieldRename> rename_fields;
  std::vector<FieldSet> set_fields;
  Timeouts timeouts;  ///< 已与全局超时合并
};

/// 对客户端暴露的一个模型名，以及它的目标列表。
struct ModelConfig {
  std::string name;  ///< 客户端请求中的 model；只包含字母、数字和 "._:/-"
  std::vector<TargetConfig> targets;  ///< 按顺序切换，不为空
};

/// 完整的配置快照。
struct Snapshot {
  ServerConfig server;
  Timeouts timeouts;
  std::vector<Provider> providers;
  ProxyConfig proxy;
  std::vector<ModelConfig> models;

  /// 按名字查找；找不到返回 nullptr。返回的指针与快照的生命周期相同。
  [[nodiscard]] const Provider* find_provider(std::string_view name) const;
  [[nodiscard]] const ModelConfig* find_model(std::string_view name) const;
};

}  // namespace llmgw::config
