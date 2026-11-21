# core

## 职责

所有模块共用的基础类型与接口：`RequestContext`、`Error`、`ResponseStream`、`Chunk`、`Target`、`MetricsRecorder` 等，以及版本信息。

## 不负责什么

- 任何具体实现和 I/O
- 依赖任何第三方网络库

## 对外接口

`llmgw/core/version.h`（版本号，由 CMake 的项目版本生成）；其余公共类型和接口在任务 0.4（#4）中设计。

公开头文件放在 `include/llmgw/core/`，其他模块只能使用这里的内容。

## 依赖

无（与 [tools/module_deps.json](../../tools/module_deps.json) 一致）

## 线程模型

只有类型定义，没有运行时状态。

## 相关任务

0.4 (#4)
