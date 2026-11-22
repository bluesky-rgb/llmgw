# 真实上游的响应样本（任务 1.8）

由 `tools/provider-probe/probe.py` 用真实 key 录制，去掉了 `id`、`created` 等每次都不同的值，不包含任何请求头、响应头和 key。

- `index.json`：每个样本的 HTTP 状态码和 Content-Type
- `<上游>/*.sse`、`<上游>/*.json`：响应体原文

`tests/unit/stream/provider_samples_test.cpp` 让每个样本经过 stream 层，检查：原样转发、与分块方式无关、用量提取、去掉 usage 块、MiniMax 业务错误码的映射。

更新样本：运行探测工具后，把输出目录中的 `samples/` 复制到这里。
