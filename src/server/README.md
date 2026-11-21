# server

## 职责

Worker、Acceptor、Session（唯一的编排者）、HTTP 编解码、Keep-Alive、过载保护、三个端口、优雅退出。

## 不负责什么

- 任何业务决策（鉴权、路由、重试等都交给对应模块）

## 对外接口

任务 1.3（#10）、3.2（#28）、3.3（#29）中设计。

公开头文件放在 `include/llmgw/server/`，其他模块只能使用这里的内容。

## 依赖

`core`、`config`、`observability`、`middleware`、`router`、`executor`（与 [tools/module_deps.json](../../tools/module_deps.json) 一致）

## 线程模型

每个 Worker 一个线程和一个 `io_context`；每条连接一个 Session 协程。

## 相关任务

1.3 (#10)、3.2 (#28)、3.3 (#29)
