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
  /// 到这个上游的最大连接数（所有 Worker 合计）。每个 Worker 的上限 = ⌈总数 ÷ Worker 数⌉
  /// （design.md 5.4；按每个 Worker 计算会让多核机器上的连接数成倍放大）
  std::uint32_t max_connections = 256;
  /// 校验该上游证书用的 CA 文件（PEM）；为空时用系统证书库
  std::string ca_file;
  std::optional<ErrorMapping> error_mapping;
};

/// HTTP 代理的地址和认证信息。
struct ProxyUrl {
  Url url;               ///< 只支持 http://（到代理本身的连接是明文的）
  std::string username;  ///< 已做百分号解码；为空表示不认证
  /// ⚠️ 不能写入日志、指标或任何输出
  std::string password;

  friend bool operator==(const ProxyUrl&, const ProxyUrl&) = default;
};

/// 出站代理。
struct ProxyConfig {
  std::optional<ProxyUrl> https;  ///< 所有上游默认使用的代理；没有值表示不用代理
  /// 按上游名覆盖；值为空（配置中写 "direct"）表示这个上游不走代理
  std::map<std::string, std::optional<ProxyUrl>> per_provider;
  /// 不走默认代理的主机（来自 NO_PROXY 环境变量，只在 https 为 "env" 时读取）。
  /// 已转为小写、去掉前导的 "*." 和 "."；"*" 表示全部
  std::vector<std::string> no_proxy;
};

/// 到上游的连接池（design.md 5.4）。
struct UpstreamConfig {
  Seconds idle_timeout{60};  ///< 空闲连接超过这个时间就关闭

  friend bool operator==(const UpstreamConfig&, const UpstreamConfig&) = default;
};

/// 重试规则（design.md 6.1，任务 1.7）。
struct RetryConfig {
  /// 是否允许重试“结果不确定”的失败：请求已经发出之后超时或断开、上游返回 500、504 等。
  /// 上游可能已经处理并计费，默认不重试（返回 504 或原样返回），避免重复扣费
  bool retry_on_ambiguous = false;

  friend bool operator==(const RetryConfig&, const RetryConfig&) = default;
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

/// 一个网关 key（客户端用来访问网关的 key，任务 2.2）。配置中只有它的哈希，没有 key 本身。
struct KeyConfig {
  std::string id;    ///< 标识：日志、指标、用量中只出现它
  std::string hash;  ///< "sha256:" + 64 位小写十六进制（加载时已经转成小写）

  friend bool operator==(const KeyConfig&, const KeyConfig&) = default;
};

/// 中间件链中的一项（任务 2.1）。插件名是否存在、插件自己的配置是否正确，由 middleware 模块校验
/// （config 不依赖 middleware，不知道有哪些插件）。
struct PipelineEntry {
  std::string name;
  /// 这一项的 "config" 对象的 JSON 文本，没有写时为 "{}"。插件在自己的实现中解析（只在启动时）
  std::string config_json = "{}";
  std::string path;  ///< 在配置文件中的位置，比如 "pipeline[1]"（错误提示用）

  friend bool operator==(const PipelineEntry&, const PipelineEntry&) = default;
};

/// 完整的配置快照。
struct Snapshot {
  ServerConfig server;
  Timeouts timeouts;
  std::vector<Provider> providers;
  ProxyConfig proxy;
  UpstreamConfig upstream;
  RetryConfig retry;
  std::vector<ModelConfig> models;
  /// 中间件链，按执行顺序（on_complete 按相反的顺序）。为空表示不启用任何中间件
  std::vector<PipelineEntry> pipeline;
  /// 网关 key。只有 pipeline 中启用了 auth 插件时才会校验
  std::vector<KeyConfig> keys;

  /// 按名字查找；找不到返回 nullptr。返回的指针与快照的生命周期相同。
  [[nodiscard]] const Provider* find_provider(std::string_view name) const;
  [[nodiscard]] const ModelConfig* find_model(std::string_view name) const;
  /// 这个上游使用的代理：per_provider 优先；其次是默认代理（主机名匹配 no_proxy 时不用）。
  /// 返回 nullptr 表示直连。
  [[nodiscard]] const ProxyUrl* proxy_for(const Provider& provider) const;
};

}  // namespace llmgw::config
