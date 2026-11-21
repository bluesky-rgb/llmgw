# accounting

## 职责

价格表、费用计算、预算账本 `BudgetLedger`（先冻结后结算、原子持久化）、用量聚合。

## 不负责什么

- 限流（属于 middleware 的 ratelimit 插件）
- 报表展示（属于 admin）

## 对外接口

任务 2.4（#21）、2.8（#25）中设计。

公开头文件放在 `include/llmgw/accounting/`，其他模块只能使用这里的内容。

## 依赖

`core`、`config`、`observability`（与 [tools/module_deps.json](../../tools/module_deps.json) 一致）

## 线程模型

`BudgetLedger` 全局一个，临界区极短；用量聚合每个 Worker 一份。

## 相关任务

2.4 (#21)、2.8 (#25)
