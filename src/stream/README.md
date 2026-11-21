# stream

## 职责

把上游的响应体包装成 `ResponseStream`：SSE 解析与重组、usage 提取、业务错误码检查。

## 不负责什么

- 网络读写（属于 upstream）
- 决定是否重试（属于 executor）

## 对外接口

任务 1.5（#12）中设计。

公开头文件放在 `include/llmgw/stream/`，其他模块只能使用这里的内容。

## 依赖

`core`、`observability`（与 [tools/module_deps.json](../../tools/module_deps.json) 一致）

## 线程模型

请求级，只在处理该请求的协程中使用。

## 相关任务

1.5 (#12)
