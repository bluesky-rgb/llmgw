# app

## 职责

程序入口和命令行（serve、check、keygen、reload、status、usage、chat），负责组装所有模块。

## 不负责什么

- 任何业务逻辑

## 对外接口

`llmgw` 可执行文件（`build/src/app/llmgw`）：

| 命令 | 状态 |
|---|---|
| `llmgw check [-c 配置文件] [--skip-env]` | ✅ 校验配置文件（任务 1.1）。退出码：0 有效，1 有错误，2 参数错误或文件读不了 |
| `llmgw version` | ✅ |
| `llmgw serve` 等 | 任务 1.9（#16）、3.4（#30）。在此之前，运行网关请用 `prototype/` 构建出的 `build/prototype/llmgw` |

公开头文件放在 `include/llmgw/app/`，其他模块只能使用这里的内容。

## 依赖

目前：`core`、`config`。最终：`core`、`config`、`observability`、`upstream`、`stream`、`accounting`、`router`、`provider`、`middleware`、`executor`、`server`、`admin`（与 [tools/module_deps.json](../../tools/module_deps.json) 一致）

## 线程模型

主线程。

## 相关任务

1.9 (#16)、3.4 (#30)
