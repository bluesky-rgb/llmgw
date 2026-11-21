# coroutine-proxy：协程并发模型的技术验证

> 这是任务 0.6 的验证代码，**不是产品代码**，不会直接进入 `src/`。
> 设计、验证方法与完整数据见 [docs/tasks/0.6-coroutine-demo.md](../../docs/tasks/0.6-coroutine-demo.md)。

它是一个约 500 行的流式 HTTP 代理，用来验证 llmgw v1.0 并发方案中风险最大的几件事：

| 要验证的 | 结果 |
|---|---|
| 每核一个线程 + `SO_REUSEPORT` + 每条连接一个协程 | ✅ 5000 条并发流全部成功 |
| 拉取循环的背压 | ✅ 客户端 100 KB/s 读 9.6 MB 的流，代理内存保持 4.6 MB 不变 |
| 客户端断开后立即取消上游 | ✅ 中位数 < 1 ms（原型 v0.1：流式中 203 ms；首字前根本不取消） |
| 总超时、空闲超时 | ✅ 没发响应头时返回 504，发了之后断开连接 |
| 每条流的内存 | ✅ 约 37 KB（目标 ≤ 100 KB） |
| ASan + UBSan、TSan | ✅ 没有报告 |

## 代码结构

| 文件 | 内容 |
|---|---|
| `main.cpp` | 参数解析，启动 N 个 Worker，处理退出信号 |
| `worker.h` | Worker：一个线程 + 一个只给自己用的 `io_context` + 一个 `SO_REUSEPORT` 的 acceptor |
| `session.h` | Session：每条连接一个协程。核心是 `relay()` 和 `pump()` |
| `common.h` | 参数、统计计数、命名空间别名 |
| `verify/` | 验证脚本（慢速客户端、取消时延、sanitizer 回归） |

**最值得读的是 `session.h` 的 `relay()`**：一个请求只做一次四方竞速 ——

```cpp
co_await (pump(st) || client_closed(client_) || timer_fired(deadline) || idle_watch(...));
```

`pump` 是读上游、写客户端的拉取循环；另外三个分别等待客户端断开、总超时、空闲超时。谁先结束就按谁处理，其余的由 Asio 自动取消。

## 构建

Boost 不在默认构建中，需要打开 `LLMGW_BUILD_SPIKE`：

```bash
cmake -S . -B build-spike -G Ninja -DLLMGW_BUILD_SPIKE=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build-spike
# 已经有 Boost 1.92.0 源码时，可以跳过下载：
#   -DFETCHCONTENT_SOURCE_DIR_BOOST=/path/to/boost-1.92.0
```

## 运行

```bash
# 1. 启动 mock 上游（任务 0.5）
(cd tools/mock-upstream && go run . -listen 127.0.0.1:18081) &

# 2. 启动 demo
build-spike/spike/coroutine-proxy/coroutine-proxy \
    --listen 127.0.0.1:18100 --upstream 127.0.0.1:18081 \
    [--workers 0] [--total-timeout 600s] [--idle-timeout 60s]

# 3. 访问
curl -N http://127.0.0.1:18100/v1/chat/completions \
     -d '{"model":"mock:chunks=5,interval=200ms","stream":true}'
curl http://127.0.0.1:18100/_proxy/stats
```

`--workers 0` 表示等于 CPU 核数。`/_proxy/stats` 返回当前会话数、累计取消上游的次数、超时次数和进程内存。

## 复现验证

```bash
V=spike/coroutine-proxy/verify
# 背压：每秒读 100 KB，持续 6 秒
python3 -I $V/slow_reader.py 127.0.0.1 18100 "mock:chunks=50000,interval=0s" 100 6
# 取消时延：读到第一块后断开，重复 20 次
python3 -I $V/cancel_latency.py 18100 "mock:chunks=100000,interval=100ms" "" 20
# 首字前断开（只收到响应头）
python3 -I $V/cancel_latency.py 18100 "mock:ttft=60s" "" 10 HTTP/1.1
# 响应头前断开
python3 -I $V/cancel_before_header.py 18100
# 5000 条并发流（loadcheck 见 tools/mock-upstream/cmd/loadcheck）
loadcheck -url http://127.0.0.1:18100 -streams 5000 -chunks 100 -interval 100ms -ramp 4s
# sanitizer 回归：先用 -DCMAKE_CXX_FLAGS=-fsanitize=... 构建，再运行
LOADCHECK=/path/to/loadcheck $V/sanitize_run.sh <二进制路径> /tmp/asan.log
```

## 已知限制（demo 有意不做，正式实现时处理）

- **每个请求新建一条上游连接**：连接池在任务 1.4 实现
- **只支持 HTTP 明文上游**：TLS 在阶段 1 实现
- **使用 HTTP 管线化（pipelining）的客户端会被误判为断开**：正式实现中用 `MSG_PEEK` 区分“有数据”和“已关闭”
- **`HEAD` 请求、`204`/`304` 响应没有特殊处理**：大模型 API 不使用这些情况
- **统计计数是全局原子变量**：正式实现中按 Worker 分片，不需要原子操作
