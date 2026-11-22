# provider

## 职责

把“客户端的请求 + 选中的一个目标”变成“发给这个上游的请求”（改写请求体、请求头、路径），发出一次尝试，把上游的响应包装成 `ResponseStream`。执行链路 executor → **provider** → upstream → stream 中连接两头的一层。v1.0 只有 OpenAI 兼容协议。

## 不负责什么

- 选择目标、重试、切换、计算时限 → executor（1.7）。provider 只做一次尝试，时限由调用者给出
- `X-LLMGW-Provider`、`X-LLMGW-Attempts` 响应头，每次尝试前重置 `ctx->usage` / `ctx->ttft` → executor（1.7）
- 连接、DNS、TLS、代理 → upstream；SSE 解析、usage、业务错误码 → stream

## 对外接口

`include/llmgw/provider/provider.h`：

```cpp
auto prepared = provider::prepare(ctx->request, provider, target);  // 纯计算；请求体不合法时 400
if (!prepared) return prepared.error();
auto s = co_await provider::attempt(client, &provider, std::move(*prepared), deadlines, ctx);
if (!s) { /* s.error() 是 upstream::AttemptError：stage + maybe_sent，executor 据此判断能否重试 */ }
// *s 是 std::unique_ptr<ResponseStream>
```

- **改写顺序**：内置改写（`model` 换成目标的模型名；流式请求加上 `stream_options.include_usage = true`）→ `drop_fields` → `rename_fields` → `set_fields`。内置改写与配置的规则在同一条线上（一条是另一条的前缀）时，让给配置
- **规则之间不能有歧义**（配置加载时检查）：规则的路径不能经过被改名或者被设置的字段；两条改名不能指向同一个目标；以 `model`、`stream` 开头的路径不能改写
- **没有被改写路径经过的值逐字节保留**（大整数、转义、空白都不变）。改写路径经过的部分不是合法的 JSON 时返回 400
- **usage 块**：发给上游的请求要求了 usage、而客户端没有要求时（键解码后判断，重复的键任意一项为 true 即为要求了），stream 去掉上游的 usage 块，客户端看到的与没有网关时一样
- **请求头**：`Authorization: Bearer <密钥>`、`Content-Type`、`Accept`、`User-Agent: llmgw/<版本>`。客户端的请求头不转发
- **响应头白名单**（不区分大小写，保留上游的写法）：`Content-Type`、`Cache-Control`、`Retry-After`、`retry-after-ms`、`x-should-retry`（executor 在结果不确定时改为 `false`）；上游的 `x-request-id` / `request-id` 改名为 `X-LLMGW-Upstream-Request-Id`（两个都有时只转发第一个）。其余（`Set-Cookie`、`openai-*`、上游的 `x-ratelimit-*`、逐跳头等）丢弃
- **`attempt()` 的错误**：发送失败、配置了 `error_mapping` 时预读出错，都原样返回 upstream 的 `AttemptError`（预读出错时 `maybe_sent = true`：请求已经发出）。读响应体中途的错误由 `next()` 返回

## 内部结构

| 文件 | 内容 |
|---|---|
| `rewrite.h/.cpp` | 改写器：规则树（按路径合并所有规则）+ 基于字节位置的重建（simdjson on-demand 的 `raw_json`） |
| `provider.cpp` | `prepare()`、`attempt()`、响应头过滤、`upstream::Body` → `stream::Source` 的适配 |

设计、测量和边界情况见 [docs/tasks/1.6-provider.md](../../docs/tasks/1.6-provider.md)。

## 依赖

- 模块：`core`、`config`、`observability`、`upstream`、`stream`（与 [tools/module_deps.json](../../tools/module_deps.json) 一致）
- 第三方：simdjson

## 线程模型

请求级：只在处理该请求的 Worker 线程上使用。`prepare()` 是纯函数。

## 性能

请求体改写（Release，丢掉每条消息的一个字段 + 内置改写）：3 KB 约 0.004 ms，93 KB 约 0.06 ms，934 KB 约 0.6 ms。同样的改写用 DOM（解析 + 修改 + 序列化）：934 KB 约 10 ms。

## 已知限制

- 不会创建数组：`messages[].name` 这样的设置只对已有的元素生效
- 没有被改写路径经过的部分只做结构上的扫描，不做完整校验：其中不合法的 JSON 原样发给上游，由上游返回 400
- 重复的键：每一个同名项都被改写
- 改名 `stream_options` 的目标：内置的 `include_usage` 让给配置，上游不会返回 usage（由预算模块估算）

## 相关任务

1.6 (#13)
