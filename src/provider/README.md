# provider

## 职责

协议适配器（v1.0 只有 OpenAI 兼容）、请求改写（`drop_fields` / `rename_fields` / `set_fields`）、响应检查。

## 不负责什么

- 连接管理（属于 upstream）
- 决定是否重试（属于 executor）

## 对外接口

任务 1.6（#13）中设计。

公开头文件放在 `include/llmgw/provider/`，其他模块只能使用这里的内容。

## 依赖

`core`、`config`、`observability`、`upstream`、`stream`（与 [tools/module_deps.json](../../tools/module_deps.json) 一致）

## 线程模型

请求级。

## 相关任务

1.6 (#13)
