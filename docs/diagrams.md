# llmgw 分析与设计图

这些图用来回答从“为什么做”到“怎么做”的各个问题。建议按顺序阅读：

| # | 图 | 回答的问题 | 阶段 |
|---|---|---|---|
| 1 | [目标分解图](#1-目标分解图) | 我们到底要达成什么？ | 需求分析 |
| 2 | [系统上下文图](#2-系统上下文图) | 系统的边界在哪？跟谁打交道？ | 需求分析 |
| 3 | [用例图](#3-用例图) | 谁用这个系统？用来做什么？ | 需求分析 |
| 4 | [组件图](#4-组件图) | 系统内部分成哪几块？依赖关系是什么？ | 架构设计 |
| 5 | [类图](#5-类图) | 核心抽象和接口长什么样？ | 详细设计 |
| 6 | [时序图](#6-时序图) | 一个请求在各模块之间怎么流转？ | 详细设计 |
| 7 | [活动图：重试决策](#7-活动图重试决策) | 出错时到底重试还是不重试？ | 详细设计 |
| 8 | [状态图](#8-状态图) | 熔断器和请求有哪些状态？怎么转换？ | 详细设计 |
| 9 | [数据流图：Token 与费用](#9-数据流图token-与费用) | 钱是怎么算出来、记下来的？ | 详细设计 |
| 10 | [部署图](#10-部署图) | 实际跑起来是什么样？ | 部署设计 |
| 11 | [甘特图](#11-甘特图) | 什么时候做完什么？ | 项目计划 |

> 并发与连接方案（线程、协程、连接池、背压、预算预留）的 8 张配图讲解见 [concurrency-explained.md](concurrency-explained.md)。

> 图中的设计已经吸收了[问题分析](design.md)中发现的问题：防重复扣费的重试规则、总超时、熔断，以及“为什么用 C++”的定位。

---

## 1. 目标分解图

从一个总目标拆成产品目标和个人目标，再落到可以测量的指标上。后面所有的设计都应该能追溯到这张图上的某个节点；追溯不到的功能，说明不该做。

```mermaid
mindmap
  root((llmgw<br/>C++ 轻量大模型网关))
    产品目标
      统一接入
        一个 OpenAI 兼容接口
        云端和本地模型混合使用
      稳定可靠
        自动故障切换
        熔断
        不重复扣费
      成本可控
        Token 与费用统计
        按 key 设预算
      易于部署
        单个二进制文件
        10 分钟完成部署
    技术目标
      单机 1 万条并发流
      每条流内存占用低
      网关额外延迟 P99 小于 1ms
      长时间运行无泄漏
    个人目标
      3 个月内拿到工作
      可量化的性能数据
      每个决策都能讲清为什么
      技术文章与开源影响力
```

---

## 2. 系统上下文图

把 llmgw 当成一个黑盒，只看它和外部的关系。实线是请求方向，虚线是可选依赖。

```mermaid
flowchart LR
    Dev["👩‍💻 应用开发者<br/>用 OpenAI SDK 调用"]
    App["业务应用<br/>聊天机器人 / Agent / 内部工具"]
    Ops["🛠 运维 / 管理员<br/>配置模型、密钥、预算"]
    EndUser["👤 终端用户"]

    GW(["llmgw<br/>大模型网关"])

    Cloud["☁️ 云端模型<br/>OpenAI · MiniMax · 小米 MiMo"]
    Local["🖥 本地模型<br/>llama.cpp server"]
    Mon["📊 监控<br/>Prometheus · Grafana"]
    Redis[("Redis<br/>多实例共享状态")]

    EndUser --> App
    Dev -. 开发和调试 .-> App
    App -->|"OpenAI 格式请求"| GW
    Ops -->|"配置文件 / Admin API"| GW
    GW -->|"转发请求"| Cloud
    GW -->|"转发请求"| Local
    Mon -->|"抓取 /metrics"| GW
    GW -. 限流计数和预算 .-> Redis
```

**边界说明：** 网关自身不做 TLS 终结（交给前面的 Nginx 或负载均衡），不存储对话内容，不做用户管理。

---

## 3. 用例图

Mermaid 没有标准的 UML 用例图，这里用流程图模拟：左边是参与者，圆角框是用例。

```mermaid
flowchart LR
    App["🧑‍💻 业务应用"]
    Ops["🛠 管理员"]
    Prom["📊 监控系统"]

    subgraph GW["llmgw 系统边界"]
        UC1(["发起对话补全<br/>流式 / 非流式"])
        UC2(["查询可用模型"])
        UC3(["自动故障切换"])
        UC4(["鉴权"])
        UC5(["限流与预算检查"])
        UC6(["配置模型路由"])
        UC7(["管理 API key 与预算"])
        UC8(["热加载配置"])
        UC9(["查看用量与费用"])
        UC10(["采集指标"])
    end

    App --> UC1
    App --> UC2
    Ops --> UC6
    Ops --> UC7
    Ops --> UC8
    Ops --> UC9
    Prom --> UC10

    UC1 -. 包含 .-> UC4
    UC1 -. 包含 .-> UC5
    UC1 -. 扩展 .-> UC3
```

“包含”表示每次都会执行；“扩展”表示只在特定条件下发生（上游失败时才会切换）。

---

## 4. 组件图

网关内部的模块和依赖方向。依赖只能从上往下，不能反过来；这样替换某一层（比如把 cpp-httplib 换成 Asio）时影响范围最小。

```mermaid
flowchart TB
    subgraph L1["接入层"]
        Server["server<br/>HTTP 接入 · SSE 输出 · 优雅退出"]
        Admin["admin<br/>热加载 · 用量查询"]
    end

    subgraph L2["策略层"]
        Chain["middleware<br/>中间件链"]
        Auth["auth"]
        RL["ratelimit"]
        Budget["budget"]
        Cache["cache（P2）"]
        Chain --> Auth & RL & Budget & Cache
    end

    subgraph L3["路由与执行层"]
        Router["router<br/>模型名 → 目标列表"]
        Exec["executor<br/>重试决策 · 总超时 · 取消"]
        CB["circuit_breaker<br/>每个上游一个"]
        Exec --> CB
    end

    subgraph L4["上游与协议层"]
        Adapter["provider<br/>OpenAI 兼容 · Anthropic"]
        Pool["upstream<br/>连接池 · HTTP 客户端"]
        Stream["stream<br/>SSE 解析 · usage 提取"]
        Adapter --> Pool
        Adapter --> Stream
    end

    subgraph Base["基础设施（所有层都可以使用）"]
        Config["config<br/>不可变快照 · 原子替换"]
        Obs["observability<br/>指标 · 日志"]
        Store["storage<br/>内存 / Redis"]
        Acct["accounting<br/>价格表 · 费用计算"]
    end

    Server --> Chain
    Admin --> Config
    Chain --> Router --> Exec --> Adapter
    RL --> Store
    Budget --> Store
    Budget --> Acct
```

---

## 5. 类图

核心抽象。接口都设计成异步（返回 `awaitable`），这样无论底层选择协程还是回调，上层都不需要改。

```mermaid
classDiagram
    direction LR

    class RequestContext {
        +string request_id
        +string key_id
        +string model
        +bool stream
        +json body
        +TimePoint deadline
        +CancelToken cancel
        +TokenUsage usage
    }

    class Middleware {
        <<interface>>
        +name() string
        +on_request(ctx) awaitable~optional~Reply~~
        +on_complete(ctx, outcome) awaitable~void~
    }
    class AuthMiddleware
    class RateLimitMiddleware {
        -TokenBucket rpm
        -TokenBucket tpm
    }
    class BudgetMiddleware
    Middleware <|.. AuthMiddleware
    Middleware <|.. RateLimitMiddleware
    Middleware <|.. BudgetMiddleware

    class MiddlewareChain {
        -vector~Middleware~ items
        +run(ctx) awaitable~optional~Reply~~
    }
    MiddlewareChain o-- Middleware

    class Router {
        +resolve(model) vector~Target~
    }
    class Target {
        +string provider
        +string upstream_model
    }
    Router ..> Target

    class Executor {
        +execute(ctx, targets, sink) awaitable~Outcome~
        -should_retry(failure) bool
    }
    class CircuitBreaker {
        -State state
        -SlidingWindow window
        +allow() bool
        +on_success()
        +on_failure()
    }
    Executor --> CircuitBreaker : 每个上游一个

    class ProviderAdapter {
        <<interface>>
        +build_request(ctx, target) HttpRequest
        +convert_response(body) string
        +make_transcoder() StreamTranscoder
    }
    class OpenAICompatAdapter
    class AnthropicAdapter
    ProviderAdapter <|.. OpenAICompatAdapter
    ProviderAdapter <|.. AnthropicAdapter
    Executor --> ProviderAdapter

    class UpstreamPool {
        +acquire(host) awaitable~Connection~
        +release(conn)
    }
    ProviderAdapter ..> UpstreamPool

    class StateStore {
        <<interface>>
        +incr(key, delta, ttl) awaitable~int64~
        +get(key) awaitable~optional~int64~~
    }
    class MemoryStore
    class RedisStore
    StateStore <|.. MemoryStore
    StateStore <|.. RedisStore
    RateLimitMiddleware --> StateStore
    BudgetMiddleware --> StateStore

    class ConfigManager {
        -atomic~shared_ptr~Config~~ current
        +snapshot() shared_ptr~Config~
        +reload(path) bool
    }
```

---

## 6. 时序图

### 6.1 非流式请求：第一个上游 503，切换到第二个

```mermaid
sequenceDiagram
    autonumber
    participant C as 客户端
    participant S as Server
    participant M as 中间件链
    participant E as Executor
    participant CB as 熔断器
    participant A as 上游 A
    participant B as 上游 B

    C->>S: POST /v1/chat/completions
    S->>M: on_request(ctx)
    M-->>S: 放行
    S->>E: execute(ctx, [A, B])
    E->>CB: A.allow()?
    CB-->>E: 允许
    E->>A: 请求
    A-->>E: 503 Service Unavailable
    E->>CB: A.on_failure()
    Note over E: 503 表示上游没有处理请求，可以安全重试
    E->>CB: B.allow()?
    CB-->>E: 允许
    E->>B: 请求
    B-->>E: 200 + usage
    E->>CB: B.on_success()
    E-->>S: Outcome(200, usage)
    S->>M: on_complete(ctx) 扣预算、记指标
    S-->>C: 200 响应
```

### 6.2 流式请求：客户端中途断开

```mermaid
sequenceDiagram
    autonumber
    participant C as 客户端
    participant S as Server
    participant E as Executor
    participant U as 上游

    C->>S: POST stream=true
    S->>E: execute(ctx)
    E->>U: 请求（自动注入 include_usage）
    U-->>E: 200 响应头
    E-->>S: 状态码已确定
    S-->>C: 200 text/event-stream
    loop 逐块转发
        U-->>E: data: {...}
        E-->>S: 转发，旁路解析 usage
        S-->>C: data: {...}
    end
    C--xS: 连接断开
    S->>E: ctx.cancel()
    E->>U: 立即关闭上游连接，停止生成
    Note over E,U: 不再读取上游，避免继续产生费用
    E-->>S: Outcome(aborted, 已产生的 usage)
    S->>S: 按已产生的用量记账，记录 client_aborted
```

---

## 7. 活动图：重试决策

这是防止**重复扣费**的关键：请求一旦已经发给上游，上游可能已经在生成内容并计费，这时盲目重试会付两次钱。

```mermaid
flowchart TD
    Start(["上游尝试失败"]) --> Sent{"请求体已经<br/>完整发出？"}

    Sent -->|"否：DNS 失败 / 连接失败 / TLS 失败"| Safe["✅ 可以安全重试"]
    Sent -->|"是"| Resp{"收到响应头了吗？"}

    Resp -->|"否：读取超时 / 连接被重置"| Ambig["⚠️ 结果不确定<br/>上游可能已经在计费"]
    Ambig --> Cfg{"配置中允许<br/>不确定重试？"}
    Cfg -->|"否（默认）"| Fail["❌ 返回 504 给客户端"]
    Cfg -->|"是"| Safe

    Resp -->|"是"| Code{"状态码"}
    Code -->|"429 / 502 / 503"| Safe
    Code -->|"500 / 504"| Ambig
    Code -->|"其他 4xx"| Pass["↩️ 原样返回，不重试"]

    Safe --> Budget{"还有剩余目标，<br/>且没超过总超时？"}
    Budget -->|"是"| Next["记录失败，熔断器计数<br/>尝试下一个目标"]
    Budget -->|"否"| Final["❌ 返回 502<br/>upstream_unavailable"]

    Next --> Breaker{"下一个目标的<br/>熔断器打开了吗？"}
    Breaker -->|"是，跳过"| Budget
    Breaker -->|"否"| Try(["发起请求"])
```

---

## 8. 状态图

### 8.1 熔断器

```mermaid
stateDiagram-v2
    [*] --> Closed
    Closed: 关闭（正常放行）
    Open: 打开（全部拒绝，直接跳过该上游）
    HalfOpen: 半开（只放一个探测请求）

    Closed --> Closed: 请求成功
    Closed --> Open: 窗口内失败率超过 50%<br/>或连续失败 5 次
    Open --> HalfOpen: 冷却时间到<br/>默认 30 秒，每次失败后翻倍，上限 5 分钟
    HalfOpen --> Closed: 探测成功，冷却时间重置
    HalfOpen --> Open: 探测失败，冷却时间翻倍
```

### 8.2 一个请求的生命周期

```mermaid
stateDiagram-v2
    [*] --> Received
    Received: 已接收
    Checked: 已通过检查
    Attempting: 正在尝试上游
    Streaming: 正在流式输出
    Completed: 成功完成
    Rejected: 被拒绝
    Failed: 失败
    Aborted: 客户端中止

    Received --> Rejected: 鉴权失败 401<br/>限流 429<br/>预算不足 402
    Received --> Checked: 中间件全部放行
    Checked --> Attempting: 路由得到目标列表
    Attempting --> Attempting: 可安全重试的失败<br/>切到下一个目标
    Attempting --> Failed: 目标用完 / 总超时 / 不确定的失败
    Attempting --> Completed: 非流式成功
    Attempting --> Streaming: 流式响应头到达
    Streaming --> Completed: 收到 [DONE]
    Streaming --> Failed: 上游中途断开，追加错误事件
    Streaming --> Aborted: 客户端断开
    Completed --> [*]
    Rejected --> [*]
    Failed --> [*]
    Aborted --> [*]
```

无论以哪种终态结束，都会执行 `on_complete`：按实际用量记账，并记录指标。

---

## 9. 数据流图：Token 与费用

```mermaid
flowchart LR
    U["上游响应"] -->|"非流式：body.usage"| Ext["usage 提取"]
    U -->|"流式：最后一块 usage"| Ext
    U -->|"流式但没有 usage"| Est["按字符数估算<br/>标记为 estimated"]
    Est --> Ext

    Price[("价格表<br/>按模型：输入单价 / 输出单价")] --> Calc
    Ext -->|"prompt_tokens<br/>completion_tokens"| Calc["费用计算<br/>输入 × 单价 + 输出 × 单价"]

    Calc -->|"本次费用"| Store[("StateStore<br/>key:日 / key:月 累计")]
    Calc --> Metrics["Prometheus 指标<br/>tokens_total · cost_total"]
    Calc --> Log["请求日志<br/>request_id + 用量 + 费用"]

    Store -->|"下一个请求进来时"| BudgetCheck{"预算检查"}
    BudgetCheck -->|"未超出"| Allow["放行"]
    BudgetCheck -->|"超出"| Act["拒绝（402）<br/>或降级到便宜模型"]
```

---

## 10. 部署图

### 10.1 单机部署（v1.0 的主要场景）

```mermaid
flowchart TB
    subgraph Host["一台服务器 · docker compose"]
        Nginx["Nginx<br/>TLS 终结 · 443"]
        GW["llmgw<br/>8080"]
        Llama["llama.cpp server<br/>8081 · GPU 可选"]
        Prom["Prometheus<br/>9090"]
        Graf["Grafana<br/>3000"]
        Nginx --> GW
        GW --> Llama
        Prom -->|"抓取 /metrics"| GW
        Graf --> Prom
    end
    Apps["内部应用"] -->|"HTTPS"| Nginx
    GW -->|"HTTPS"| Cloud["云端模型 API"]
```

### 10.2 多实例部署（需要 Redis 共享状态）

```mermaid
flowchart LR
    Apps["应用"] --> LB["负载均衡"]
    LB --> G1["llmgw #1"]
    LB --> G2["llmgw #2"]
    LB --> G3["llmgw #3"]
    G1 & G2 & G3 --> Redis[("Redis<br/>限流计数 · 预算累计")]
    G1 & G2 & G3 --> Cloud["云端模型 API"]
```

网关本身无状态，任何一个实例挂掉都不影响其他实例；熔断器状态各个实例独立维护，不需要共享。

---

## 11. 甘特图

```mermaid
gantt
    title llmgw 开发计划
    dateFormat YYYY-MM-DD
    axisFormat %m-%d

    section M1 原型
    原型实现（已完成）            :done,    m1a, 2026-11-20, 1d
    设计文档与分析图              :done,    m1b, 2026-11-20, 1d
    修复原型的关键问题            :active,  m1c, 2026-11-23, 5d

    section M2 性能底座
    Asio 协程流式代理 demo        :         m2a, after m1c, 4d
    接入层与上游层重写            :         m2b, after m2a, 6d
    连接池                        :         m2c, after m2a, 4d
    第一份压测报告                :milestone, m2d, after m2b, 0d

    section M3 稳定性与成本
    熔断器与重试决策              :         m3a, after m2b, 4d
    限流与预算                    :         m3b, after m3a, 5d
    Grafana 看板                  :         m3c, after m3b, 2d

    section M4 完善
    配置热加载                    :         m4a, after m3c, 3d
    docker compose 一键部署       :         m4b, after m4a, 2d

    section M5 发布
    压测报告与技术文章            :         m5a, after m4b, 4d
    v1.0 发布                     :milestone, m5b, after m5a, 0d
```

> 甘特图中已经把 Redis 多实例和 Anthropic 适配移出 v1.0，缩小了范围（见问题分析）。
