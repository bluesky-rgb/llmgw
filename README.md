# llmgw：用 C++ 写的轻量大模型网关

[![CI](https://github.com/bluesky-rgb/llmgw/actions/workflows/ci.yml/badge.svg)](https://github.com/bluesky-rgb/llmgw/actions/workflows/ci.yml)

**llmgw 是一个可自部署的大模型网关。** 它把 OpenAI（GPT）、MiniMax、小米 MiMo 以及本地 llama.cpp 等多家模型统一到同一个兼容 OpenAI 的接口后面，负责故障自动切换、流式转发、成本管控和监控。一个二进制文件加一份配置文件就能运行，数据不出自己的服务器。

> **项目状态：** v0.1 原型已经可以运行（[使用手册](docs/usage.md)）。v1.0 的设计已经完成，正在开发中，当前阶段是 **M2：性能底座**（[里程碑](docs/design.md#9-里程碑)）。

## 解决什么问题

| 问题 | llmgw 的做法 |
|---|---|
| 各家模型的接口不统一 | 对外只提供一个 OpenAI 兼容接口，现有 SDK 改一下 `base_url` 就能用；各家的参数差异由网关按目标处理 |
| 某一家宕机或限流，业务就挂了 | 按模型配置备用链路，自动切换；连续失败的上游会被熔断；**按规则重试，不会重复扣费** |
| 费用失控 | 按 key 统计 Token 和费用，设置日、月预算，超出时拒绝或降级到便宜的模型 |
| 上游 key 散落在各个应用里 | 应用只拿网关的 key，上游的真实 key 只保存在网关上 |
| 看不到运行情况 | Prometheus 指标（包括首字延迟 TTFT）、JSON 日志、Grafana 看板 |

**为什么用 C++？** 流式输出一条连接要挂一两分钟，网关真正的压力是**大量的长连接**。C++ 加协程可以让单机撑住上万条并发流，每条流只占几十 KB 内存，没有 GC 停顿，部署时也不需要任何语言运行时。详见[设计文档 1.2 节](docs/design.md#12-定位)。

## 架构

![llmgw 整体架构](docs/images/01-overall-architecture.svg)

- **每核一个线程，每条连接一个协程**：线程负责用满多个 CPU 核，协程负责让每个核同时照看大量连接
- **线程之间尽量不共享数据**：连接池、熔断状态、指标计数都是每个线程一份，不需要加锁
- **请求依次经过五层**：鉴权、限流、预算 → 路由 → 执行（重试、熔断） → 协议适配 → 流式转发（带背压）
- **可插拔**：微内核 + 插件，中间件、路由策略、协议适配等 7 个扩展点由配置组装；新增 OpenAI 兼容的厂商只需改配置
- **稳定**：入口、流量、调用、资源、进程五层纵深防护，每种故障模式都有对应的应对措施和自动化测试

不熟悉这些概念的话，建议先读 [并发方案图解](docs/concurrency-explained.md)，里面用 8 张图从头讲起。

## 范围

| v1.0 支持 | 明确不做 | 以后再做（v1.x） |
|---|---|---|
| OpenAI 兼容的对话接口（流式 / 非流式） | TLS 终结（交给 Nginx 等反向代理） | Anthropic 协议适配 |
| 首批验证 OpenAI（GPT）、MiniMax、小米 MiMo；其他 OpenAI 兼容上游（llama.cpp、vLLM 等） | 存储对话内容 | 多实例部署（Redis） |
| 模型路由、故障切换、熔断、超时 | 用户体系、Web 管理后台 | Embeddings、Responses API |
| 连接池、背压、过载保护 | 多租户 SaaS 与计费 | 精确缓存与语义缓存 |
| 鉴权、限流、费用核算、预算 | 内容审核 | 智能路由、HTTP/2 |
| 指标、日志、热加载、优雅退出 | Windows / macOS 生产运行 | OpenTelemetry |

完整的范围表（以及原型的实现情况）见 [设计文档第 2 节](docs/design.md#2-范围与边界)。

## 客户端怎么接入

**不需要额外的 SDK**：OpenAI 官方 SDK、LangChain、Dify 等任何支持 OpenAI 接口的工具，把 `base_url` 换成网关地址、`api_key` 换成网关 key 即可。

```python
from openai import OpenAI
client = OpenAI(base_url="http://llmgw.internal:8080/v1", api_key="sk-gw-...")
client.chat.completions.create(model="smart", messages=[{"role": "user", "content": "你好"}])
```

模型名使用网关的逻辑名（如 `smart`、`fast`），以后网关调整背后的厂商时，应用代码不用改。错误处理、重试、超时、用量查询等见 [客户端接入指南](docs/client-guide.md)。

## 快速开始（v0.1 原型）

```bash
# 编译（需要 CMake ≥ 3.20、C++17 编译器、OpenSSL 3）
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build && ctest --test-dir build

# 配置并运行（示例配置已包含 GPT、MiniMax 国内站、MiMo Token Plan）
cp config.example.json config.json
export OPENAI_API_KEY=sk-...  MINIMAX_API_KEY=...  MIMO_API_KEY=tp-...
./build/llmgw --config config.json

# 调用
curl http://127.0.0.1:8080/v1/chat/completions \
  -H "Authorization: Bearer sk-gw-change-me" -H "Content-Type: application/json" \
  -d '{"model":"smart","stream":true,"messages":[{"role":"user","content":"你好"}]}'
```

`smart` 会先用 GPT，失败后依次切换到 MiniMax、MiMo。配置字段、接入本地 llama.cpp、监控指标、Docker 等详见 [原型使用手册](docs/usage.md)。

## 文档导航

| 想了解 | 文档 |
|---|---|
| 项目目标、范围、架构、工程设计、各项机制 | [设计文档](docs/design.md) |
| **应用怎么接入网关**：示例、模型名、请求头、错误与重试、超时 | [客户端接入指南](docs/client-guide.md) |
| 代码怎么组织、有哪些规范、怎么防止代码腐化 | [代码架构与可维护性规范](docs/code-architecture.md) |
| 怎样做到加厂商、加功能不改核心代码 | [可扩展与可插拔设计](docs/extensibility.md) |
| 会出哪些故障、怎么防护、怎么降级、怎么验证 | [稳定性设计](docs/stability.md) |
| 监控哪些指标、记录哪些日志、怎么出用量报表 | [监控、日志与统计设计](docs/observability.md) |
| GPT、MiniMax、MiMo 三家的接入方式、差异和验证清单 | [上游厂商适配说明](docs/providers.md) |
| 线程、协程、连接池、背压、预算预留是怎么回事 | [并发方案图解](docs/concurrency-explained.md) |
| 目标分解、上下文、用例、组件、类、时序、状态、部署、计划等分析图 | [分析与设计图](docs/diagrams.md) |
| 每个设计决策的背景、选择和代价 | [设计决策记录](docs/decisions.md) |
| 怎么编译、配置和运行当前的原型 | [原型使用手册](docs/usage.md) |
| 上线前还要做哪些事（按优先级） | [生产就绪检查清单](docs/production-readiness.md) |
| 还有哪些问题没定、哪些文档要补 | [设计文档第 11 节](docs/design.md#11-待补充与开放问题) |

## 路线图

- [x] **M1 原型与设计**：v0.1 原型、设计文档、分析图
- [ ] **M2 性能底座**：协程重写接入层和上游层、连接池、背压、超时与取消、第一份压测报告
- [ ] **M3 稳定性与成本**：重试规则、熔断、限流、费用核算、预算、TTFT 指标、Grafana 看板
- [ ] **M4 运维完善**：热加载、过载保护、优雅退出、key 哈希、docker compose
- [ ] **M5 发布 v1.0**：压测报告、技术文章、GitHub Release 与 Docker 镜像

## 当前代码结构

```
src/          v0.1 原型：config、gateway（cpp-httplib）、sse、metrics、url、logger、main
tests/        单元测试与端到端测试（进程内 mock 上游 + 真实网关）
docs/         设计文档、图解、分析图、决策记录、使用手册
.github/      CI：Release、ASan + UBSan、TSan 三套构建
```

v1.0 的目标目录结构见 [设计文档 4.1 节](docs/design.md#41-目录结构v10-目标)。

## License

[MIT](LICENSE)
