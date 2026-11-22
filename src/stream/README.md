# stream

## 职责

把上游的响应体包装成 core 的 `ResponseStream`：流式响应按 **完整的 SSE 事件** 切块转发（不会把一个事件切成两半，切块方式不影响结果），顺路提取 usage 和首字延迟；配置了业务错误码映射的上游，检查响应体中的错误码并改写状态码。

## 不负责什么

- 网络读写 → upstream。字节来源用 stream 自己的 `Source` 接口，provider（1.6）把 `upstream::Body` 适配过来
- 在请求中加上 `stream_options.include_usage` → provider（1.6）
- 根据状态码决定重试 → executor（1.7）
- 拿不到 usage 时的估算 → 预算模块（2.x）

## 对外接口

| 头文件 | 内容 |
|---|---|
| `stream.h` | `open()`、`Source`、`ErrorMapping`、`Options` |
| `sse_parser.h` | `SseParser`：增量 SSE 解析器（纯计算），`SseEvent` |

```cpp
auto s = co_await stream::open(std::move(head), std::move(source), ctx,
                               {.strip_usage_chunk = injected_include_usage, .error_mapping = mapping});
// s 是 std::unique_ptr<ResponseStream>：Session 循环调用 next()
```

- **按 SSE 解析的条件**：状态码 2xx 且 `Content-Type` 是 `text/event-stream`。其他情况（包括流式请求得到了 JSON 错误体）原样转发
- **业务错误码**：配置了 `error_mapping` 且状态码是 2xx 时，先读到第一个带 `data` 的事件（流式）或整个响应体（非流式），命中时把状态码改成映射后的值。之后与真实的 HTTP 错误走同样的重试规则
- **usage**：取最后一个非空的 `usage`（`prompt_tokens`、`completion_tokens`、`completion_tokens_details.reasoning_tokens`）；非流式响应从响应体提取（副本不超过 32 MB）
- **首字延迟**：第一个带 `data` 的事件被交出的时间 − `received_at`；注释、心跳不算

## 内部结构

| 文件 | 内容 |
|---|---|
| `sse_parser.cpp` | 按行解析（三种换行、注释、字段规则、BOM）、事件大小的上限、结束时补上缺少的空行 |
| `inspect.h/.cpp` | 对单个 JSON 的检查：usage、业务错误码（simdjson on-demand） |
| `stream.cpp` | `SseStream`、`BodyStream`、`open()` |

设计、边界情况和验证见 [docs/tasks/1.5-stream.md](../../docs/tasks/1.5-stream.md)。

## 依赖

- 模块：`core`（与 [tools/module_deps.json](../../tools/module_deps.json) 一致；不依赖 `config` 和 `upstream`）
- 第三方：simdjson

## 线程模型

请求级：`ResponseStream` 只在处理该请求的 Worker 线程上使用。simdjson 的解析器是线程局部的。

## 性能

典型的 OpenAI 流（每个事件约 220 字节，每个事件都带 `"usage": null`）：每个事件约 600 ns，包括 usage 检查。1 MB 的单个事件逐字节到达：9 ms（线性）。

## 已知限制

- 只用 `\r` 换行的流：一个事件要等下一个事件的第一个字节（或者响应体结束）才交出（任务设计第 4 节）
- 配置了 `error_mapping` 的上游：客户端要等到第一个事件（或整个非流式响应体）才收到响应头
- 非流式响应超过 32 MB 时拿不到 usage
- 预读（配置了 `error_mapping` 时）的总量超过 `max_event_bytes`（4 MB）时放弃检查
- 结尾的处理：缺少空行时补上；没有换行的 `data: [DONE]` 补全；`[DONE]` 之后或注释中的半行丢弃；没有任何事件的响应体原样转发；其他在行中间断开的情况报错

## 相关任务

1.5 (#12)
