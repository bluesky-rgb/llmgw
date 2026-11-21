# llmgw：用 C++ 写的轻量大模型网关

[![CI](https://github.com/bluesky-rgb/llmgw/actions/workflows/ci.yml/badge.svg)](https://github.com/bluesky-rgb/llmgw/actions/workflows/ci.yml)

一个兼容 OpenAI 接口的大模型网关，一个二进制文件就能跑。它把 OpenAI、DeepSeek、本地 llama.cpp 等多家模型统一到同一个接口后面，负责**故障自动切换、流式转发、鉴权、Token 统计和监控指标**。

```
你的应用 / OpenAI SDK
        │  POST /v1/chat/completions   (model: "smart")
        ▼
   ┌─────────┐   1. 云端模型 A  ──✗ 5xx / 429 / 连接失败
   │  llmgw  │──▶ 2. 云端模型 B  ──✗
   └─────────┘   3. 本地 llama.cpp ──✓ 流式返回
        │
        └── /metrics → Prometheus / Grafana
```

## 功能（v0.1）

- **兼容 OpenAI 接口**：`/v1/chat/completions`、`/v1/models`，现有 SDK 只需改 `base_url`
- **按模型配置故障切换链**：上游返回 5xx、429 或连接失败时，按顺序自动切到下一个目标
- **流式输出（SSE）**：数据逐块实时转发，不缓冲；**在第一个字节发出之前**同样可以故障切换
- **状态码如实转发**：流式请求中上游返回 400，客户端收到的也是 400，而不是 200 加一条错误事件
- **模型名映射**：对外叫 `smart`，对内可以是 `gpt-4o`、`deepseek-chat` 或本地的 `qwen2.5-7b`
- **网关鉴权**：客户端使用网关自己的 key，上游的真实 key 不会暴露给应用
- **Token 统计**：从非流式响应和流式最后一块中解析 `usage`
- **Prometheus 指标**：请求数、延迟直方图、上游失败次数、Token 用量
- **结构化日志**：每个请求一行 JSON，带 `request_id`，可透传 `X-Request-Id`

## 快速开始

### 1. 编译

需要 CMake ≥ 3.20、支持 C++17 的编译器，以及 OpenSSL 3（Ubuntu 下安装 `libssl-dev`）。依赖库会自动从 GitHub 下载。

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

### 2. 配置

```bash
cp config.example.json config.json
export OPENAI_API_KEY=sk-...
export DEEPSEEK_API_KEY=sk-...
./build/llmgw --config config.json --check   # 只检查配置，不启动
./build/llmgw --config config.json
```

### 3. 调用

```bash
curl http://127.0.0.1:8080/v1/chat/completions \
  -H "Authorization: Bearer sk-gw-change-me" \
  -H "Content-Type: application/json" \
  -d '{"model":"gpt-4o-mini","stream":true,"messages":[{"role":"user","content":"你好"}]}'
```

使用 OpenAI 官方 Python SDK：

```python
from openai import OpenAI
client = OpenAI(base_url="http://127.0.0.1:8080/v1", api_key="sk-gw-change-me")
for chunk in client.chat.completions.create(
        model="gpt-4o-mini", stream=True,
        messages=[{"role": "user", "content": "你好"}]):
    print(chunk.choices[0].delta.content or "", end="")
```

## 接入本地 llama.cpp

llama.cpp 自带的 `llama-server` 提供 OpenAI 兼容接口，可以直接作为上游：

```bash
# 在 llama.cpp 目录下
./build/bin/llama-server -m models/qwen2.5-7b-instruct-q4_k_m.gguf --port 8081
```

然后在配置中加入：

```json
{ "name": "local", "base_url": "http://127.0.0.1:8081/v1", "read_timeout_seconds": 600 }
```

把 `local` 放在某个模型 `targets` 的末尾，云端全部不可用时就会落到本地模型上。

## 配置说明

| 字段 | 说明 |
|---|---|
| `listen.host` / `listen.port` | 监听地址，默认 `0.0.0.0:8080` |
| `listen.threads` | 工作线程数，默认 64。**当前版本一个流式请求占用一个线程**，所以它也是最大并发流数 |
| `api_keys` | 客户端访问网关的密钥列表；为空表示不鉴权（只建议在内网使用） |
| `providers[].name` | 上游名称，在 `models` 中引用 |
| `providers[].base_url` | 上游地址，带 `/v1`，与 OpenAI SDK 的 `base_url` 写法相同 |
| `providers[].api_key_env` | 从哪个环境变量读取上游密钥（推荐）；也可以直接写 `api_key` |
| `providers[].connect_timeout_seconds` | 连接超时，默认 10 秒 |
| `providers[].read_timeout_seconds` | 读取超时，默认 300 秒 |
| `providers[].headers` | 额外发给上游的请求头 |
| `models[].name` | 对外暴露的模型名 |
| `models[].targets` | 按顺序尝试的上游列表；`model` 不写时沿用对外模型名 |

### 什么情况会切换

| 情况 | 行为 |
|---|---|
| 连接失败、超时 | 切到下一个目标 |
| HTTP 5xx、429 | 切到下一个目标；如果已经是最后一个，原样返回 |
| 其他 4xx（参数错误等） | 不切换，原样返回。这是请求本身的问题，换上游也没用 |
| 流式输出开始后上游中断 | 无法切换，在流末尾追加一条 `error` 事件 |
| 全部目标都失败 | 返回 502，`code: upstream_unavailable` |

## 接口

| 方法 | 路径 | 说明 |
|---|---|---|
| POST | `/v1/chat/completions` | 聊天补全，支持 `stream: true` |
| GET | `/v1/models` | 列出配置中的模型 |
| GET | `/health` | 健康检查 |
| GET | `/metrics` | Prometheus 指标 |

响应头 `X-LLMGW-Provider` 表示这次请求最终由哪个上游处理，`X-Request-Id` 用于和日志对应。

## 监控指标

```
llmgw_requests_total{model, provider, status, stream}
llmgw_request_duration_ms_bucket{model, le}        # 端到端延迟直方图
llmgw_upstream_failures_total{provider, reason}    # 每一次失败的上游尝试
llmgw_tokens_total{model, provider, type}          # prompt / completion
```

> 流式请求的 usage：llama.cpp 默认会在最后一块中返回；OpenAI 需要请求中带上 `"stream_options": {"include_usage": true}`。

## 代码结构

```
src/
  main.cpp       命令行入口、信号处理
  config.*       配置解析与校验（出错时说明具体哪个字段有问题）
  gateway.*      HTTP 路由、故障切换、流式转发（核心）
  sse.*          SSE 按行重组与 usage 提取
  metrics.*      线程安全的指标收集，输出 Prometheus 格式
  url.*          base_url 解析
  logger.*       JSON 结构化日志
tests/
  test_main.cpp  单元测试 + 端到端测试（进程内的假上游 + 真实网关）
docs/
  decisions.md   设计决策记录
```

## 测试

测试会在进程内启动一个假上游和一个真实网关，从外部发起 HTTP 请求，覆盖非流式、流式、5xx/429/连接失败切换、错误透传、鉴权和并发流。CI 中同时用 AddressSanitizer、UBSan 和 ThreadSanitizer 运行。

```bash
ctest --test-dir build --output-on-failure
```

## Docker

```bash
docker build -t llmgw .
docker run -p 8080:8080 -v $PWD/config.json:/etc/llmgw/config.json:ro \
  -e OPENAI_API_KEY -e DEEPSEEK_API_KEY llmgw
```

> 注意：容器内的 `127.0.0.1` 指向容器自己。如果要访问宿主机上的 llama.cpp，请使用 `host.docker.internal`，或者加上 `--network host`。

## 路线图

- [ ] **上游连接池**：目前每次请求都会新建连接（HTTPS 每次都要握手），复用连接可以明显降低延迟
- [ ] **压测报告**：使用 wrk / k6 测量网关自身增加的延迟（P50/P99）与 QPS，并和直连上游对比
- [ ] **限流与预算**：按 API key 限制 QPS、Token 数和每日费用
- [ ] **语义缓存**：相似问题直接返回缓存结果，统计命中率与节省的费用
- [ ] **熔断**：某个上游连续失败时暂时跳过它，避免每个请求都要先等它超时
- [ ] **异步 I/O**：从“每个流一个线程”改为基于 Drogon / Boost.Asio 的事件驱动模型，支撑上万并发流
- [ ] **智能路由**：简单问题走本地小模型，复杂问题走云端大模型
- [ ] **配置热加载**

## License

MIT
