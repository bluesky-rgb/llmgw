# router

## 职责

根据对外模型名得到按顺序排列的目标列表；路由策略扩展点。

## 不负责什么

- 发送请求、重试、熔断（属于 executor）

## 对外接口

任务 1.7（#14）中设计。

公开头文件放在 `include/llmgw/router/`，其他模块只能使用这里的内容。

## 依赖

`core`、`config`（与 [tools/module_deps.json](../../tools/module_deps.json) 一致）

## 线程模型

无状态，只读取配置快照。

## 相关任务

1.7 (#14)
