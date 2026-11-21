# mock-upstream

可以按需“制造各种上游故障”的假大模型服务，提供 OpenAI 兼容接口。llmgw 的集成测试、故障注入测试和压测都用它代替真实的上游：不花钱、结果可重复、能模拟真实上游很难复现的故障。

设计见 [docs/tasks/0.5-mock-upstream.md](../../docs/tasks/0.5-mock-upstream.md)。只使用 Go 标准库。

## 运行

```bash
cd tools/mock-upstream
go run . -listen 127.0.0.1:18081
```

| 参数 | 默认值 | 作用 |
|---|---|---|
| `-listen` | `127.0.0.1:18081` | 监听地址 |
| `-ttft` / `-chunks` / `-interval` | `0` / `5` / `0` | 默认的首字延迟、流式块数、块间隔 |
| `-error-rate` | `0` | 随机返回 503 的比例（0~1） |
| `-seed` | `1` | 随机数种子 |

## 用模型名控制行为

在网关配置中，把目标的上游模型名写成 `mock:参数=值,参数=值`。不以 `mock` 开头的模型名使用默认行为。**无法识别的参数会返回 400**，避免测试因拼写错误而意外通过。

| 参数 | 含义 |
|---|---|
| `status=<码>` | 返回这个 HTTP 状态码和 OpenAI 格式的错误 |
| `retry_after=<秒>` | 带上 `Retry-After` 响应头 |
| `header_delay=<时长>` | 等待这么久才返回响应头 |
| `blackhole=true` | 接受连接，但永远不返回任何数据 |
| `ttft=<时长>` | 响应头之后、第一个数据块之前的等待 |
| `chunks=<n>` / `interval=<时长>` | 流式块数和块间隔 |
| `stall_after=<n>` | 发出 n 块后停住，不再发送也不关闭连接 |
| `disconnect_after=<n>` | 发出 n 块后直接断开连接 |
| `biz_error=<码>` | HTTP 200，响应体中带 MiniMax 风格的 `base_resp.status_code` |
| `reasoning=true` | 输出中包含 `reasoning_content` |
| `usage=false` | 不返回 usage（流式时，只有请求带 `stream_options.include_usage` 才会返回） |
| `quota_requests=<n>` / `quota_tokens=<n>` | 带上 `x-ratelimit-remaining-*` 响应头 |
| `echo=true` | 把收到的请求体放进回答内容 |

时长写法：`500ms`、`2s`。

## 对应的上游故障（stability.md 3.1）

| 故障 | 模拟方式 |
|---|---|
| 上游宕机，连接被拒绝 | 连接一个没有服务监听的端口（不需要 mock） |
| 网络不通，数据包被丢弃 | `mock:blackhole=true` |
| 上游变得很慢 | `mock:header_delay=30s`，或 `mock:ttft=…`、`interval=…` |
| 上游限流 | `mock:status=429,retry_after=2` |
| 上游返回 5xx | `mock:status=500` / `502` / `503` / `504` |
| 错误码放在响应体里 | `mock:biz_error=1002` |
| 输出到一半断开 | `mock:disconnect_after=3` |
| 输出中途卡住 | `mock:stall_after=3` |
| 上游 key 失效、余额不足 | `mock:status=401` / `403` / `402` |
| 配额即将用完 | `mock:quota_requests=2,quota_tokens=100` |
| DNS 解析失败 | 使用不存在的域名（不需要 mock） |
| 出站代理故障 | 配置一个不可用的代理地址（不需要 mock） |

## 观测接口（测试用）

| 接口 | 返回 |
|---|---|
| `GET /_mock/stats` | 请求总数、按模型名的请求数、活跃 / 完成 / **被客户端中途取消**的流、内存、服务端测得的块发送偏差 |
| `GET /_mock/requests?limit=N` | 最近的 N 个请求（请求头和请求体），最多保留 200 个 |
| `POST /_mock/reset` | 清空统计和记录 |

## 性能

```bash
go run ./cmd/loadcheck -url http://127.0.0.1:18081 -streams 5000 -chunks 20 -interval 50ms
```

在 2 核机器上（客户端和 mock 在同一台机器），5000 条并发流全部成功，mock 内存峰值约 115 MB，**mock 服务端测得的块发送偏差 P99 ≤ 5 ms**。客户端测得的偏差更大（约 16 ms），主要来自同一台机器上客户端自身的 CPU 竞争。正式压测时，建议把 mock、网关、压测客户端放在不同的 CPU 核或不同的机器上。
