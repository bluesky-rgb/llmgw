# 使用手册（v0.2 Alpha）

> 本文说明当前版本的用法：新架构的 `llmgw`（任务 1.1–1.9）。v0.1 原型现在只作为压测的对照组，它的说明见 [prototype/README.md](../prototype/README.md)。
> **v0.2 是 Alpha 版：还没有客户端鉴权。** 请只在本机或可信网络中使用（见“运行”）。

## 已实现的功能

| 功能 | 状态 |
|---|---|
| OpenAI 兼容接口：`POST /v1/chat/completions`（流式、非流式）、`GET /v1/models` | ✅ |
| 按模型配置故障切换链；**重试规则保证不会重复扣费**（[design.md 6.1](design.md#61-重试与防重复扣费)） | ✅ 1.7 |
| 四种超时：连接、首字节、空闲、总；可以按目标单独设置 | ✅ 1.7 |
| 连接池（keep-alive）、DNS 缓存、HTTPS 代理 | ✅ 1.4 |
| 请求改写：替换模型名、`drop_fields`、`rename_fields`、`set_fields` | ✅ 1.6 |
| 流式响应自动要求返回用量（客户端没要求时再去掉，客户端看到的与直连相同）；MiniMax 业务错误码映射 | ✅ 1.5、1.6 |
| 背压（客户端读得慢时不再从上游拉取）、客户端断开时立即取消上游请求 | ✅ 1.3 |
| 中间件链（`pipeline`）与插件注册表；`llmgw plugins` | ✅ 2.1 |
| 客户端鉴权（网关 key，配置中只保存哈希）；`llmgw keygen` | ✅ 2.2 |
| 限流、费用核算与预算 | ⏳ 2.3、2.4 |
| 熔断、重试预算 | ⏳ 2.5 |
| Prometheus 指标、访问日志 | ⏳ 2.8 |
| 热加载、优雅退出 | ⏳ 3.1、3.3 |

## 编译

需要 CMake ≥ 3.24、GCC 13 或 Clang 17 以上（C++20）、OpenSSL 3（Ubuntu：`libssl-dev`）。Boost 等依赖在配置时自动下载（校验 SHA-256）。

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

可执行文件是 `build/src/app/llmgw`。只要网关本身时可以加 `-DLLMGW_BUILD_TESTS=OFF -DLLMGW_BUILD_PROTOTYPE=OFF`，并且只构建 `llmgw_app` 目标。

## 配置

从示例开始：[config/llmgw.example.json](../config/llmgw.example.json)（已包含 GPT、MiniMax 国内站、MiMo Token Plan 和三个模型）。完整的字段说明见 [design.md 第 7 节](design.md#7-配置)。

```bash
cp config/llmgw.example.json llmgw.json
# v0.2 还没有客户端鉴权：示例中数据面监听 0.0.0.0，直接启动会被拒绝（见“运行”）。本机使用时改为 127.0.0.1
sed -i 's/"host": "0.0.0.0"/"host": "127.0.0.1"/' llmgw.json
# 上游 key 只放在环境变量中，配置里只写变量名（api_key_env）
export OPENAI_API_KEY=sk-...  MINIMAX_API_KEY=...  MIMO_API_KEY=tp-...
./build/src/app/llmgw check -c llmgw.json      # 只校验配置；错误会指出字段的位置
```

常用的字段：

| 字段 | 说明 |
|---|---|
| `server.host` / `server.port` | 数据面的监听地址，默认 `0.0.0.0:8080`。**v0.2 中请改为 `127.0.0.1`**（见下） |
| `server.observability_listen` | `/health`、`/ready`，默认 `0.0.0.0:9100` |
| `server.workers` | Worker 线程数，`0` 表示 CPU 核数 |
| `timeouts` | `connect`、`first_byte`（流式）、`first_byte_non_stream`、`idle`、`total`，单位秒 |
| `providers[]` | `name`、`base_url`（带 `/v1`）、`api_key_env`、`max_connections`、`error_mapping`、`ca_file` |
| `proxy.https` | `"env"` 表示读取 `HTTPS_PROXY` / `NO_PROXY`；`proxy.per_provider` 可以按上游设置代理或 `"direct"` |
| `models[].targets[]` | 按顺序尝试：`provider`、`model`（上游的模型名），以及 `drop_fields`、`rename_fields`、`set_fields`、`timeouts` |
| `pipeline` | 按顺序启用的中间件插件：`["auth", {"name": "auth", "config": {"failures_per_minute": 20}}]` 两种写法。插件名拼错时启动失败并列出可用的插件（`llmgw plugins`）。目前有 `auth` |
| `keys[]` | 网关 key：`id`（日志和用量中的标识）、`hash`（`llmgw keygen` 输出的 `sha256:…`）。**配置中只能写哈希**，写了 key 本身会报错 |
| `retry.retry_on_ambiguous` | 是否重试“结果不确定”的失败（已发出请求后超时、500、504 以及 502 / 503 以外的其他 5xx），默认 `false`：可能重复扣费 |

与原型的区别：没有 `listen`（改为 `server`）、没有 `api_keys`（改为只保存哈希的 `keys`）、上游没有额外请求头（`headers`）、key 只能从环境变量读取。

## 运行

```bash
./build/src/app/llmgw serve -c llmgw.json
# llmgw 已启动：数据面 127.0.0.1:8080，管理面 127.0.0.1:9090，观测面 0.0.0.0:9100，N 个 Worker（workers: 0 时等于 CPU 核数）
```

Ctrl-C 或 SIGTERM 停止（进行中的请求会被中断；优雅退出在 3.3）。退出码：0 正常停止，1 配置错误或启动失败，2 参数错误或配置文件读不了。

**没有鉴权时的保护：** 网关持有上游 key。没有启用 `auth` 插件、数据面又监听非本机地址（不是 `127.x.x.x`、`::1`、`localhost`）时，`serve` 会**拒绝启动**并说明原因。启用鉴权（下一节）；或者确认只有可信的客户端能访问这个端口（比如容器只映射到宿主机的 `127.0.0.1`、或者在防火墙后面）时，加 `--allow-unauthenticated`。

## 鉴权

```bash
./build/src/app/llmgw keygen --id app-chatbot
# 网关 key（只显示这一次，网关不保存它，请交给使用方并妥善保管）：
# sk-gw-u3l0dU3t2SwCSZzP8SbBsKBfw3we-LK9HASaOOnjxm0
# 把下面这一项加入配置的 keys（并确认 pipeline 中有 "auth"）：
#   { "id": "app-chatbot", "hash": "sha256:fb5d1384…" }
```

配置中加上 `"pipeline": ["auth"]` 和上面的 `keys` 项，重启网关。客户端带上 `Authorization: Bearer <网关 key>`（OpenAI SDK 的 `api_key` 参数就是它）：

| 情况 | 响应 |
|---|---|
| 没有 key、key 不对、不是 `Bearer` 格式 | 401 `invalid_api_key`（`WWW-Authenticate: Bearer`、`x-should-retry: false`） |
| 同一个客户端地址 1 分钟内失败超过 20 次（IPv6 按 /64，`failures_per_minute` 可调） | 之后的失败返回 429（带 `Retry-After`）；**有效的 key 不受影响** |
| key 有效 | 放行；日志和用量中只出现 `id` |

- key 只输出到标准输出，说明输出到标准错误：`llmgw keygen --id x > key.txt` 可以直接存进文件
- 网关只保存哈希（SHA-256；key 是 256 位随机数，所以不需要慢哈希）。客户端的 key **不会转发给上游**
- 更换 key：新 key 的哈希加入 `keys` → 重启 → 应用切换 → 删除旧的那一项 → 重启（热加载在 3.1）
- `/v1/models` 也需要 key

## 调用

```bash
curl http://127.0.0.1:8080/v1/chat/completions \
  -H "Content-Type: application/json" \
  -d '{"model":"smart","stream":true,"messages":[{"role":"user","content":"你好"}]}'
```

OpenAI 官方 Python SDK（`api_key` 目前可以随便填）。可能已经扣费的错误（网关的 504、原样转发的上游 500 / 504）都带 `x-should-retry: false`，官方 SDK 不会自动重试它们；其他 SDK 的兼容性在 2.6 中验证：

```python
from openai import OpenAI
client = OpenAI(base_url="http://127.0.0.1:8080/v1", api_key="unused")
for chunk in client.chat.completions.create(
        model="smart", stream=True, messages=[{"role": "user", "content": "你好"}]):
    print(chunk.choices[0].delta.content or "", end="")
```

响应头：`X-LLMGW-Provider`、`X-LLMGW-Upstream-Model`（实际由谁处理）、`X-LLMGW-Attempts`（尝试了几次）、`X-Request-Id`（排障用；客户端可以自己带上）、`x-should-retry`（错误时：是否建议客户端自动重试）。

## 什么情况会切换

| 情况 | 默认行为 |
|---|---|
| DNS、连接、TLS 失败；连接池排队超时 | 换下一个目标 |
| 上游返回 429、502、503；401、403、余额不足 | 换下一个目标（401 / 403 是网关自己的上游 key 有问题，不会转发给客户端） |
| **已发出请求后超时或断开；上游返回 500、504 或其他 5xx（502、503 除外）** | **不重试**：504 或原样返回（带 `x-should-retry: false`）。上游可能已经处理并扣费。`retry.retry_on_ambiguous: true` 时换下一个 |
| 其他 4xx | 原样返回（请求本身有问题，换上游也没用） |
| 流式输出开始以后中断 | 不切换，在流的末尾追加一条 `error` 事件 |
| 所有目标都失败 | 有可能已扣费的尝试：504；最后一个是 429：429（带上游的 `Retry-After`）；否则 502 `upstream_unavailable` |

完整的规则表见 [design.md 6.1](design.md#61-重试与防重复扣费)。

## 接口

| 端口 | 方法 | 路径 | 说明 |
|---|---|---|---|
| 数据面 | POST | `/v1/chat/completions` | 聊天补全，支持 `stream: true` |
| 数据面 | GET | `/v1/models` | 配置中的模型 |
| 观测面 | GET | `/health` | 进程存活 |
| 观测面 | GET | `/ready` | 可以接收请求 |

## 接入本地 llama.cpp

`llama-server` 提供 OpenAI 兼容接口，可以直接作为上游。它不需要 key，但 `api_key_env` 是必填的，给它一个任意值：

```bash
./build/bin/llama-server -m models/qwen2.5-7b-instruct-q4_k_m.gguf --port 8081   # llama.cpp 目录下
export LOCAL_KEY=none
```

```json
{ "name": "local", "base_url": "http://127.0.0.1:8081/v1", "api_key_env": "LOCAL_KEY" }
```

把 `local` 放在某个模型 `targets` 的末尾，云端全部不可用时就会落到本地模型上。本地模型首字较慢时，给这个目标单独设置 `"timeouts": { "first_byte": 120 }`。

## Docker

```bash
docker build -t llmgw .
# 容器用自己的配置：server.host 必须是 0.0.0.0（监听 127.0.0.1 时，端口映射收不到连接）
cp config/llmgw.example.json llmgw.docker.json
chmod 644 llmgw.docker.json   # 镜像以非 root 用户运行，要能读到这个文件
docker run --rm -p 127.0.0.1:8080:8080 -p 127.0.0.1:9100:9100 \
  -v $PWD/llmgw.docker.json:/etc/llmgw/llmgw.json:ro \
  -e OPENAI_API_KEY -e MINIMAX_API_KEY -e MIMO_API_KEY \
  llmgw serve -c /etc/llmgw/llmgw.json --allow-unauthenticated
```

容器中监听 `0.0.0.0`，所以要加 `--allow-unauthenticated`；`-p 127.0.0.1:…` 保证只有宿主机本身能访问。不加这个参数时容器会拒绝启动并说明原因。配置文件读不了时退出码是 2。容器内的 `127.0.0.1` 指向容器自己：访问宿主机上的 llama.cpp 请使用 `host.docker.internal` 或 `--network host`。CI 中会构建这个镜像并验证这两种启动方式。

## 测试

```bash
ctest --test-dir build --output-on-failure
```

单元测试、集成测试（进程内的 Server + 测试上游）、`llmgw serve` 的进程级冒烟测试；CI 中还会用 AddressSanitizer + UBSan、ThreadSanitizer 各跑一遍，并对解析器做模糊测试。用真实 key 验证三家上游的工具见 [tools/provider-probe](../tools/provider-probe/README.md)（不在 CI 中运行）。
