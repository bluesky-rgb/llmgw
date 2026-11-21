# server

## 职责

接入层：接收客户端连接、解析 HTTP、按请求创建 `RequestContext`，交给 `Dispatcher` 得到 `ResponseStream`，再按拉取循环把响应写回客户端。同时守住入口：请求大小、读取超时、连接数上限、客户端断开与超时时的取消。

## 不负责什么

- 路由、调用上游、重试 → router / executor（通过 `Dispatcher` 接入，任务 1.7）
- 鉴权、限流、预算 → middleware（任务 2.x；Session 中在 `dispatch()` 之前调用）
- 指标输出、访问日志 → observability（任务 1.2）
- 优雅退出（等进行中的请求结束）→ 任务 3.x

## 对外接口

| 头文件 | 内容 |
|---|---|
| `server.h` | `Server`（启动、停止、实际端口）、`ServerOptions`、`WorkerInfo`、`pregrow_fd_table()` |
| `dispatcher.h` | `Dispatcher`：每个 Worker 一个实例，`dispatch(RequestContext*)` → `ResponseStream` 或错误 |

| 端口 | 接口 |
|---|---|
| 数据面 `server.host:port` | `POST /v1/chat/completions`、`GET /v1/models` |
| 观测面 `server.observability_listen` | `GET /health`、`GET /ready`（停止时 503）；`/metrics` 在 1.2 实现 |
| 管理面 `server.admin_listen` | `/admin/*` 在 3.x 实现，目前一律 404 |

## 内部结构

| 文件 | 内容 |
|---|---|
| `server.cpp` | 启动与停止；绑定三个端口；`pregrow_fd_table()` |
| `worker.h/.cpp` | Worker：一个线程 + 一个 `io_context` + 一个 `SO_REUSEPORT` 的 acceptor；连接数上限 |
| `session.h/.cpp` | Keep-Alive 循环；读请求（请求头限时、请求体要求持续有进展、大小上限）；路由；错误响应 |
| `relay.cpp` | 一次竞速（`relay ‖ 客户端断开 ‖ 总超时`）+ 拉取循环（`pump ‖ 空闲超时`）；HTTP/1.0 |
| `control_plane.h/.cpp` | 管理面和观测面，跑在单独的线程上 |
| `async_util.h` | 竞速用的辅助函数：定时器、客户端断开的检测（`MSG_PEEK`）、惰性空闲定时器 |
| `http_util.h/.cpp` | 纯函数：错误响应体、请求 ID、从请求体中取 `model`/`stream`（simdjson）、模型列表 |

设计与流程图见 [docs/tasks/1.3-server.md](../../docs/tasks/1.3-server.md)。

## 依赖

- 模块：`core`、`config`（与 [tools/module_deps.json](../../tools/module_deps.json) 一致）
- 第三方：Boost.Asio / Beast；simdjson（热路径上解析请求体）；nlohmann/json（构造错误响应和模型列表）

## 线程模型

| 线程 | 内容 |
|---|---|
| Worker × N | 数据面。每个 Worker 的 Session、Dispatcher、MetricsRecorder 只在自己的线程上使用 |
| 控制面 × 1 | 管理面、观测面 |
| 跨线程共享 | 只有两个原子变量：连接计数、“正在停止”标志；配置快照只读 |

## 已知限制

- 只关闭写方向（半关闭）的客户端会被当作断开（HTTP 客户端几乎不这样做）
- 32 MB 的请求体解析 `model`/`stream` 约 8 ms，期间这个 Worker 上的其他连接要等待。更大的上限或更严格的延迟要求下，需要把超大请求体交给 BlockingPool（design.md 第 5 节）
- 不支持 gzip 请求体、`Expect: 100-continue`、HTTP/2（production-readiness.md 6.2）

## 相关任务

1.3 (#10)
