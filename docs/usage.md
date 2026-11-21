# v0.1 原型使用手册

> 本文说明**当前 v0.1 原型**的用法。v0.1 基于 cpp-httplib（线程池阻塞模型），用于验证功能，并在 v1.0 中作为性能对照组保留。v1.0 的设计见 [design.md](design.md)，两者的配置格式会有差异。

## 原型已实现的功能

- **兼容 OpenAI 接口**：`/v1/chat/completions`、`/v1/models`，现有 SDK 只需改 `base_url`
- **按模型配置故障切换链**：上游返回 5xx、429 或连接失败时，按顺序切换到下一个目标
- **流式输出（SSE）**：逐块实时转发；在第一个字节发出之前也可以切换
- **状态码如实转发**：流式请求中上游返回 400，客户端收到的也是 400
- **模型名映射**：对外叫 `smart`，对内可以是 `gpt-4o`、`deepseek-chat` 或本地的 `qwen2.5-7b`
- **网关鉴权**：客户端使用网关自己的 key，上游的真实 key 不会暴露给应用
- **Token 统计、Prometheus 指标、JSON 结构化日志**

### 原型的已知问题

这些问题都会在 v1.0 中解决，详见 [design.md 第 2.1 节](design.md#21-v10-支持的功能)：

- 读取超时后也会切换上游，**可能造成重复扣费**
- 一个流式请求占用两个线程，并发能力受 `listen.threads` 限制
- 每个请求都新建上游连接，没有连接池
- 转发队列没有上限（没有背压），客户端很慢时内存会增长
- 只有读取超时，没有总超时；客户端断开后，上游线程最长要等到读取超时才会退出
- 没有熔断；网关 key 在配置中是明文

## 编译

需要 CMake ≥ 3.20、支持 C++17 的编译器，以及 OpenSSL 3（Ubuntu 下安装 `libssl-dev`）。依赖库会自动从 GitHub 下载。

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

离线构建时，可以指定本地源码目录：

```bash
cmake -S . -B build -DFETCHCONTENT_SOURCE_DIR_HTTPLIB=/path/to/cpp-httplib \
                    -DFETCHCONTENT_SOURCE_DIR_NLOHMANN_JSON=/path/to/json
```

## 运行

```bash
cp config.example.json config.json
export OPENAI_API_KEY=sk-...
export DEEPSEEK_API_KEY=sk-...
./build/llmgw --config config.json --check   # 只检查配置，不启动
./build/llmgw --config config.json
```

## 调用

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
| `listen.threads` | 工作线程数，默认 64。原型中一个流式请求会占用线程，所以它也限制了最大并发流数 |
| `api_keys` | 客户端访问网关的密钥列表；为空表示不鉴权（只建议在内网使用） |
| `providers[].name` | 上游名称，在 `models` 中引用 |
| `providers[].base_url` | 上游地址，带 `/v1`，与 OpenAI SDK 的 `base_url` 写法相同 |
| `providers[].api_key_env` | 从哪个环境变量读取上游密钥（推荐）；也可以直接写 `api_key` |
| `providers[].connect_timeout_seconds` | 连接超时，默认 10 秒 |
| `providers[].read_timeout_seconds` | 读取超时，默认 300 秒 |
| `providers[].headers` | 额外发给上游的请求头 |
| `models[].name` | 对外暴露的模型名 |
| `models[].targets` | 按顺序尝试的上游列表；`model` 不写时沿用对外模型名 |

### 什么情况会切换（原型的行为）

| 情况 | 行为 |
|---|---|
| 连接失败、超时 | 切到下一个目标（⚠️ v1.0 中读取超时默认不再重试，见 [design.md 6.1](design.md#61-重试与防重复扣费)） |
| HTTP 5xx、429 | 切到下一个目标；如果已经是最后一个，原样返回 |
| 其他 4xx | 不切换，原样返回 |
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

> 流式请求的 usage：llama.cpp 默认会在最后一块中返回；OpenAI 需要请求中带上 `"stream_options": {"include_usage": true}`（v1.0 会自动加上）。

## 测试

测试会在进程内启动一个假上游和一个真实网关，从外部发起 HTTP 请求，覆盖非流式、流式、5xx / 429 / 连接失败切换、错误透传、鉴权和并发流。CI 中同时使用 AddressSanitizer、UBSan 和 ThreadSanitizer 运行。

## Docker

```bash
docker build -t llmgw .
docker run -p 8080:8080 -v $PWD/config.json:/etc/llmgw/config.json:ro \
  -e OPENAI_API_KEY -e DEEPSEEK_API_KEY llmgw
```

> Dockerfile 还没有实际构建验证过。容器内的 `127.0.0.1` 指向容器自己；如果要访问宿主机上的 llama.cpp，请使用 `host.docker.internal`，或者加上 `--network host`。
