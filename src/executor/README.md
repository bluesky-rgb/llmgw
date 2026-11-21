# executor

## 职责

按目标列表执行请求：重试决策、切换、熔断器、重试预算、舱壁隔离、截止时间与取消、配额感知。

## 不负责什么

- 协议细节（属于 provider）
- 连接池（属于 upstream）

## 对外接口

任务 1.7（#14）、2.5（#22）、2.7（#24）中设计。

公开头文件放在 `include/llmgw/executor/`，其他模块只能使用这里的内容。

## 依赖

`core`、`config`、`observability`、`provider`（与 [tools/module_deps.json](../../tools/module_deps.json) 一致）

## 线程模型

熔断器、重试预算、配额记录都是每个 Worker 一份。

## 相关任务

1.7 (#14)、2.5 (#22)、2.7 (#24)
