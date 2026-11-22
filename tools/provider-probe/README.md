# 上游探测工具（任务 1.8）

用真实 key 探测 OpenAI、MiniMax、MiMo 的行为，填写 [providers.md](../../docs/providers.md) 第 5 节的验证清单，并录制回归测试用的响应样本。只用 Python 3 标准库（3.9 以上）。

## 运行

```bash
# 1. 构建网关（--gateway 会启动它，经过网关再测一遍）
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j

# 2. key 只放在环境变量中（没有设置的上游会被跳过）
export OPENAI_API_KEY=sk-...
export MINIMAX_API_KEY=...
export MIMO_API_KEY=tp-...
# 可选：模型名、MiMo 的专属地址（以控制台显示的为准）、代理
# export MIMO_BASE_URL=https://token-plan-cn.xiaomimimo.com/v1
# export OPENAI_MODEL=gpt-5.4-mini  MINIMAX_MODEL=MiniMax-M3  MIMO_MODEL=mimo-v2.6-pro
# export HTTPS_PROXY=http://127.0.0.1:7890  NO_PROXY=api.minimaxi.com,xiaomimimo.com

# 3. 运行（约 4 分钟，其中 3 分钟在等第 9 项“空闲连接多久被关闭”）
python3 tools/provider-probe/probe.py --gateway build/src/app/llmgw
```

完成后把输出目录 `probe-out-<时间>/` 整个发回来（`report.md`、`report.json`、`samples/`），并在 `report.md` 末尾的表格里填上各家控制台显示的配额（第 12 项）。

## 会做什么

- 每家上游约 20 个很小的请求（`max_completion_tokens` 16–256）；第 9 项只用 `GET /models`，不消耗额度
- 第 5 项（触发限流）默认**不做**：它会一次发出一串请求。需要时加 `--burst 20`
- `--only openai,mimo`：只测其中几家；`--idle-max 60`：缩短第 9 项的等待

## 安全

- key 只从环境变量读取；不写进任何文件、不打印
- 写任何文件之前，检查内容中没有任何一个 key：出现完整的 key 就中止并删除输出目录；出现前 8 位时替换为 `***`
- 样本只有响应体，没有请求头、响应头；`id`、`created` 等字段替换为固定值
- `--gateway` 生成的临时配置只写环境变量的名字（`api_key_env`），代理用 `"https": "env"`，运行结束后删除

## 自检

不需要 key：`python3 tools/provider-probe/probe.py --self-test --gateway build/src/app/llmgw`。在本机启动模拟三家行为的假上游，完整运行一遍并检查报告（ctest 中的 `cli.provider_probe_self_test`）。
