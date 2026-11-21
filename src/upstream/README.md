# upstream

## 职责

到上游的 HTTP/1.1 客户端：每个 Worker 一个 `Client`，内含每个上游一个的连接池和 DNS 缓存；TLS（证书和主机名校验）、HTTP 代理（`CONNECT` 隧道 / 绝对路径形式、Basic 认证、`NO_PROXY`、`direct`）。**每一种失败都说明“上游是否可能已经收到请求”**，供 executor 判断能否重试而不重复扣费（design.md 6.1）。

## 不负责什么

- 重试和切换、熔断 → executor（1.7、2.x）。唯一的例外：复用的连接写请求失败时，自动换新连接重发一次（上游一定没有收到完整的请求）
- 请求格式改写、鉴权头 → provider（1.6）
- SSE 解析 → stream（1.5）。`Body::read()` 交出去掉了分块编码的原始字节
- 响应体的空闲超时 → Session 的看门狗（1.3）

## 对外接口

| 头文件 | 内容 |
|---|---|
| `client.h` | `Client`（`send()`、`stats()`）、`Request`、`Deadlines`、`Response`、`Body`、`AttemptError`（阶段 + `maybe_sent`）、`make_tls_contexts()`、`per_worker_limit()` |

```cpp
auto r = co_await client.send(provider, std::move(request), {.connect = 10s, .first_byte = 60s});
if (!r) {
  // r.error().stage、r.error().maybe_sent：false 表示可以安全地换一个目标
  co_return r.error().to_error();  // 503 / 502 / 504
}
std::string buf;
while (true) {
  auto more = co_await r->body->read(&buf);   // 追加到 buf
  if (!more) { /* 中途出错，maybe_sent = true */ }
  if (!*more) break;                           // 正常结束：连接自动还回连接池
}
```

## 内部结构

| 文件 | 内容 |
|---|---|
| `client.cpp` | `send()`：连接阶段（排队 → 复用或新建）、交换阶段（写请求 → 读响应头）各一次竞速 |
| `pool.h/.cpp` | 连接池：空闲连接（后进先出）、名额（`Lease`，析构时一定归还）、排队与交接、空闲清理 |
| `connection.h/.cpp` | 一条连接：TCP 或 TLS；复用前的健康检查（`recv(MSG_PEEK)`） |
| `establish.h/.cpp` | 新建连接：DNS → TCP →（代理 `CONNECT`）→（TLS 握手） |
| `message.h/.cpp` | 请求报文（保留的请求头、请求头注入的过滤、绝对路径形式）；读最终响应头（跳过 1xx）；`Body` 的实现 |
| `dns.h/.cpp` | DNS 缓存（60 秒；失败时用旧结果）；可取消的等待 |
| `tls.h/.cpp` | 全局共享的 TLS 上下文 |

设计、失败分类和验证见 [docs/tasks/1.4-upstream.md](../../docs/tasks/1.4-upstream.md)。

## 依赖

- 模块：`core`、`config`（与 [tools/module_deps.json](../../tools/module_deps.json) 一致）
- 第三方：Boost.Asio / Beast；OpenSSL 3（系统库）

## 线程模型

| 对象 | 线程 |
|---|---|
| `Client`、连接池、DNS 缓存、`Body` | 只在创建它的 Worker 线程上使用，不加锁 |
| `TlsContexts` | 启动时创建，之后只读，所有 Worker 共享 |
| DNS 解析 | Asio 内部的专用线程（`getaddrinfo`） |

## 指标

| 指标 | 标签 | 含义 |
|---|---|---|
| `llmgw_upstream_connections_total` | `provider`、`kind` = `reused` / `new` / `queue_timeout` | 每次取连接的结果；复用率 = reused ÷ (reused + new) |
| `llmgw_pool_connections` | `provider`、`state` = `idle` / `busy` | 每个 Worker 上报自己的值，汇总时相加（1.2） |

## 已知限制

- 只支持 HTTP/1.1：并发流数 = 连接数，`max_connections` 同时是舱壁（HTTP/2 在 v1.x）
- 只支持 HTTP 代理（不支持 HTTPS 代理、SOCKS）；`NO_PROXY` 不支持 CIDR
- 按顺序尝试解析出的地址，没有 Happy Eyeballs：IPv6 黑洞时要等到连接超时
- 复用前检查之后、请求到达之前上游恰好关闭了连接：报告为 `maybe_sent = true` 且 `reused_without_response = true`，默认不重试（任务设计第 6 节）
- 活跃的连接各有一块最多 64 KB 的读缓冲区（Beast 要求响应头在连续的缓冲区中）；空闲连接不占
- 不做 TLS 会话复用
- 每个 Worker 至少 1 条连接：Worker 数超过 `max_connections` 时，实际连接数会超过它
- DNS：过期后先用旧结果、在后台刷新；没有旧结果时的失败记住 1 秒

## 相关任务

1.4 (#11)
