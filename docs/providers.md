# 上游厂商适配说明

v1.0 首批验证的上游是项目作者手上已有的三家 API：**OpenAI（GPT）**、**MiniMax（国内站）**、**小米 MiMo（Token Plan）**。三家都提供 OpenAI 兼容的 Chat Completions 接口，所以 v1.0 不需要额外的协议适配器，只需要处理一些参数和行为上的差异。

> 信息整理自各家官方文档（2026 年 11 月）。各家接口变化很快，以官方文档为准；标有 ⚠️ 的项目需要用真实 key 验证。

## 1. 基本信息

| | OpenAI | MiniMax（国内站） | 小米 MiMo（Token Plan） |
|---|---|---|---|
| `base_url` | `https://api.openai.com/v1` | `https://api.minimaxi.com/v1` | `https://token-plan-cn.xiaomimimo.com/v1` ⚠️ 以控制台显示的专属地址为准 |
| 鉴权 | `Authorization: Bearer` | `Authorization: Bearer` | `Authorization: Bearer` 或 `api-key`，网关统一使用 Bearer |
| key 格式 | `sk-…` | — | `tp-…`（个人版）/ `ttp-…`（团队版） |
| 示例模型 | `gpt-5.6-sol`、`gpt-5.4-mini` | `MiniMax-M3` ⚠️ 国内站的模型列表需要确认 | `mimo-v2.6-pro` |
| 计费方式 | 按 token 计费 | 按 token 计费 | **订阅套餐，按额度限量调用** |
| 环境变量（约定） | `OPENAI_API_KEY` | `MINIMAX_API_KEY` | `MIMO_API_KEY` |

注意：
- MiniMax 的国内站（minimaxi.com）和国际站（minimax.io）是两套独立的账号体系，**key 不能混用**
- MiMo 的按量付费（`sk-…`，`api.xiaomimimo.com`）和 Token Plan（`tp-…`）的地址也不同，同样不能混用

## 2. 行为差异

| 方面 | OpenAI | MiniMax | MiMo | 对网关的影响 |
|---|---|---|---|---|
| 流式返回用量 | 支持 `stream_options.include_usage` | 支持，用量只在最后一块返回 | ⚠️ 直连接口未在文档中说明（通过阿里云百炼调用时支持） | 网关自动加上 `include_usage`；需要验证 MiMo |
| 推理（思考）内容 | 不返回推理文本 | 默认开启思考，推理文本放在非标准字段 `reasoning_content` | 推理文本放在 `reasoning_content` | 原样透传，不改动 |
| 推理内容回传 | — | — | **思考模式下的多轮工具调用，必须在历史消息里原样回传 `reasoning_content`，否则返回 400** | 切换到其他厂商时可能需要删除这个字段（见 3.1） |
| 最大输出长度参数 | 推理模型要求用 `max_completion_tokens` | `max_tokens` 已弃用，改用 `max_completion_tokens` | 使用 `max_completion_tokens` | 统一改写为 `max_completion_tokens`（见 3.2） |
| `temperature` | 支持 | **不生效**，固定为 1 | 支持 | 无需处理 |
| `reasoning_effort` 的取值 | 与 MiniMax 不同 | `low` / `medium` / `high` / `xhigh` / `max`；`none` 会返回 400 | ⚠️ 未确认 | 不同厂商之间切换时可能返回 400（见 3.3） |
| 错误格式 | HTTP 状态码 + `error` 对象 | 业务错误码放在响应体的 `base_resp.status_code` 里，例如 1002 = 触发限流，1004 = 鉴权失败，1008 = 余额不足 | ⚠️ 未确认 | ⚠️ 需要确认 MiniMax 出错时的 HTTP 状态码是否正确；如果出错时 HTTP 状态码仍为 200，网关必须检查 `base_resp`（见 3.4） |
| 首字延迟 | 推理模型首字较慢 | 默认思考强度为 `max`，首字可能很慢 | 默认开启思考 | 超时时间要按模型单独设置（见 3.5） |
| 额外的响应字段 | — | `input_sensitive`、`output_sensitive`、`base_resp` 等 | — | 原样透传 |

## 3. 网关需要做的适配

### 3.1 跨厂商切换时的参数清理

切换上游时，同一个请求体会发给另一家厂商。某一家专有的字段，另一家可能不认，甚至直接返回 400。比如客户端按照 MiMo 的要求在历史消息里带上了 `reasoning_content`，切换到 OpenAI 时就可能出错。

**方案：** 每个目标可以配置 `drop_fields`，转发前删掉指定的字段，支持删除消息里的字段：

```json
{ "provider": "openai", "model": "gpt-5.6-sol",
  "drop_fields": ["messages[].reasoning_content", "reasoning_split", "thinking"] }
```

### 3.2 参数改写

**方案：** 每个目标可以配置 `rename_fields` 和 `set_fields`：
- `rename_fields`：例如把 `max_tokens` 改名为 `max_completion_tokens`
- `set_fields`：强制设置某个参数，例如给 MiniMax 设置 `"reasoning_effort": "medium"`，降低首字延迟和费用

### 3.3 参数取值不兼容

`reasoning_effort` 这类参数在不同厂商之间取值不同。v1.0 不做取值之间的自动映射（规则太多，也容易出错），而是：
- 用 `drop_fields` 在切换到备用厂商时删掉这个参数，让备用厂商使用默认值
- 文档中提示用户：需要精确控制推理强度的话，在网关的模型配置里用 `set_fields` 统一设置

### 3.4 响应体中的业务错误码

如果某家厂商出错时 HTTP 状态码仍然是 200，只在响应体里放错误码，网关的重试规则就会失效。

**方案：** 每个厂商可以配置 `error_mapping`，把响应体中的业务错误码映射成标准的 HTTP 状态码，再套用重试规则：

```json
{ "name": "minimax",
  "error_mapping": { "field": "base_resp.status_code",
                     "codes": { "1002": 429, "1004": 401, "1008": 402, "1001": 504, "1013": 500 } } }
```

只有非流式响应和流式响应的第一块需要检查；如果实际验证后发现 MiniMax 出错时的 HTTP 状态码本身就是正确的，这个功能可以推迟到 v1.x。

### 3.5 按模型设置超时

推理模型的首字延迟可能长达几十秒，非流式请求更要等到整个回答生成完毕才返回。所以：
- 超时时间支持按目标单独配置，覆盖全局默认值
- 非流式请求的“等待响应头”超时默认值设为 300 秒，比流式请求长

### 3.6 订阅套餐的计费

MiMo 的 Token Plan 是固定订阅费、按额度限量调用的，没有“每个 token 多少钱”。

**方案：** 价格表支持 `"billing": "subscription"`：
- 这类上游的费用记为 0，但照常统计 token 用量
- 可以配置套餐额度（比如每天或每月的调用次数、token 数），快用完时在指标中提示
- 套餐额度通常比按量付费更紧，更容易触发 429；网关会根据 `Retry-After` 暂时跳过它（见 design.md 6.2）

### 3.7 上游鉴权失败、余额不足

上游返回 401、403 或余额不足，是**网关自己的上游 key 出了问题**，不是客户端的错。如果把 401 原样返回给客户端，客户端会以为自己的网关 key 不对。

**方案：** 这类错误视为“该上游不可用”，切换到下一个目标；全部失败时返回 502。同时记录 `llmgw_upstream_auth_errors_total` 指标，方便设置告警。

## 4. 推荐的模型路由示例

```json
"models": [
  { "name": "smart", "targets": [
      { "provider": "openai",  "model": "gpt-5.6-sol",
        "drop_fields": ["messages[].reasoning_content"] },
      { "provider": "minimax", "model": "MiniMax-M3" },
      { "provider": "mimo",    "model": "mimo-v2.6-pro" } ] },

  { "name": "fast", "targets": [
      { "provider": "openai",  "model": "gpt-5.4-mini",
        "drop_fields": ["messages[].reasoning_content"] },
      { "provider": "mimo",    "model": "mimo-v2.6-pro" } ] },

  { "name": "mimo", "targets": [ { "provider": "mimo", "model": "mimo-v2.6-pro" } ] }
]
```

- `smart`：GPT 优先，失败后依次切换到 MiniMax、MiMo
- `fast`：便宜、快速的模型优先
- `mimo`：只用 MiMo，适合需要回传 `reasoning_content` 的 Agent 场景，避免切换到其他厂商后出错

## 5. 验证清单

用真实 key 逐项验证，结果更新到本文档。每项都写成自动化的集成测试，默认不在 CI 中运行（会产生费用），需要手动触发。

| # | 验证项 | OpenAI | MiniMax | MiMo |
|---|---|---|---|---|
| 1 | 非流式、流式基本调用 | ☐ | ☐ | ☐ |
| 2 | `stream_options.include_usage` 是否返回用量 | ☐ | ☐ | ☐ |
| 3 | 用量中是否包含推理 token（`completion_tokens_details.reasoning_tokens`） | ☐ | ☐ | ☐ |
| 4 | 错误的 key：HTTP 状态码和响应体 | ☐ | ☐ | ☐ |
| 5 | 触发限流：HTTP 状态码、是否有 `Retry-After` | ☐ | ☐ | ☐ |
| 6 | 请求里带上其他厂商的专有字段（如 `reasoning_content`）是否返回 400 | ☐ | ☐ | ☐ |
| 7 | `max_tokens` 和 `max_completion_tokens` 是否都能用 | ☐ | ☐ | ☐ |
| 8 | 首字延迟（默认推理强度下） | ☐ | ☐ | ☐ |
| 9 | 空闲连接多久会被服务端关闭（决定连接池的 `idle_timeout`） | ☐ | ☐ | ☐ |
| 10 | 录制真实的流式响应样本，作为回归测试数据（去掉敏感内容） | ☐ | ☐ | ☐ |

## 参考资料

- MiniMax：[OpenAI 兼容接口（国内站）](https://platform.minimaxi.com/docs/api-reference/text-openai-api) · [Chat Completions 参考](https://platform.minimax.io/docs/api-reference/text-chat-openai)
- 小米 MiMo：[首次调用 API](https://mimo.mi.com/docs/zh-CN/quick-start/summary/first-api-call) · [OpenAI 兼容接口](https://mimo.mi.com/docs/zh-CN/api/chat/openai-api) · [深度思考](https://mimo.mi.com/docs/zh-CN/quick-start/usage-guide/other/deep-thinking)
- OpenAI：[模型列表](https://developers.openai.com/api/docs/models) · [更新日志](https://developers.openai.com/api/docs/changelog)
