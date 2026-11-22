# executor

## 职责

按目标列表依次尝试，按重试规则表（design.md 6.1）决定“把结果交给客户端”还是“换下一个目标”，**保证请求可能已经被上游计费时不重试**。以后（2.5、2.7）还有熔断器、重试预算、舱壁隔离、配额感知。

## 不负责什么

- 协议细节、请求改写（属于 provider）；连接池（属于 upstream）
- 总超时、空闲超时（属于 Session：executor 只把选中目标的 `timeouts.idle` 写进 `ctx->upstream_idle`）
- 流式输出开始以后的中断：`execute()` 已经返回，错误由 `next()` 交给 Session（ADR-003）

## 对外接口

`include/llmgw/executor/executor.h`：

```cpp
executor::Executor ex({.client = &client, .metrics = recorder});   // 每个 Worker 一个
auto s = co_await ex.execute(ctx, route->targets);
// 成功：ResponseStream（2xx，或原样转发的上游响应），带 X-LLMGW-Provider、X-LLMGW-Attempts
// 失败：400（请求体不合法）、502（所有上游都失败）、504（可能已经计费，不要重试）
```

**重试规则表**（`classify.h`，纯函数；完整定义见[任务设计第 4 节](../../docs/tasks/1.7-router-executor.md)）：

| 结果 | 默认 | `retry.retry_on_ambiguous` |
|---|---|---|
| 请求没有发出（DNS、连接、TLS、排队超时、发送失败） | 换下一个 | 同左 |
| 429、502、503；401、403、402；3xx | 换下一个 | 同左 |
| 已发出后超时 / 断开 | 504 | 换下一个 |
| 500、504、其他 5xx | 原样返回 | 换下一个 |
| 其他 4xx、2xx | 原样返回 | 同左 |

- 所有目标都失败：之前有不确定的尝试 → 504；最后一次是 429 → 429，502 / 503 → 502，都带上游的 `Retry-After`（不转发上游的响应体）；否则 502。错误信息只有失败的分类，不包含上游名和地址（design.md 6.10）
- 配置了 `error_mapping` 时，返回之前的预读受目标的 `timeouts.idle` 限制（provider 实现）
- 每次尝试：从原始请求体按这个目标的规则改写；重置 `ctx->usage`、`ctx->ttft`；连接、首字节时限按目标的配置并且不超过剩余的总时间；追加一条 `ctx->attempts`，上报 `llmgw_upstream_attempts_total`、`llmgw_upstream_duration_seconds`，401 / 403 上报 `llmgw_upstream_auth_errors_total`
- 被取消（客户端断开、总超时）：记一条 `cancelled`，不计为上游的失败，不再尝试下一个

## 依赖

`core`、`config`、`observability`、`upstream`（ADR-008）、`provider`（与 [tools/module_deps.json](../../tools/module_deps.json) 一致）

## 线程模型

每个 Worker 一个 `Executor`，只在该 Worker 的线程上使用；以后的熔断器、重试预算也是每个 Worker 一份。

## 相关任务

1.7 (#14)、2.5 (#22)、2.7 (#24)
