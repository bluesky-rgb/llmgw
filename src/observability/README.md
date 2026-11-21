# observability

## 职责

指标分片与汇总、异步的结构化日志、`MetricsRecorder` 接口的实现。

## 不负责什么

- 告警规则（交给 Prometheus）
- 用量统计（属于 accounting）

## 对外接口

任务 1.2（#9）、2.8（#25）中设计。

公开头文件放在 `include/llmgw/observability/`，其他模块只能使用这里的内容。

## 依赖

`core`（与 [tools/module_deps.json](../../tools/module_deps.json) 一致）

## 线程模型

指标分片每个 Worker 一份，不加锁；日志由独立线程写出，队列有上限。

## 相关任务

1.2 (#9)、2.8 (#25)
