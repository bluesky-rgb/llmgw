# app

## 职责

程序入口和命令行（serve、check、keygen、reload、status、usage、chat），负责组装所有模块。

## 不负责什么

- 任何业务逻辑

## 对外接口

任务 1.9（#16）、3.4（#30）中实现。在此之前，`llmgw` 可执行文件来自 `prototype/`。

公开头文件放在 `include/llmgw/app/`，其他模块只能使用这里的内容。

## 依赖

`core`、`config`、`observability`、`upstream`、`stream`、`accounting`、`router`、`provider`、`middleware`、`executor`、`server`、`admin`（与 [tools/module_deps.json](../../tools/module_deps.json) 一致）

## 线程模型

主线程。

## 相关任务

1.9 (#16)、3.4 (#30)
