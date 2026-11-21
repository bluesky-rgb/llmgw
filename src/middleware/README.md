# middleware

## 职责

中间件接口与中间件链、插件注册表；内置鉴权、限流、预算三个插件。

## 不负责什么

- 路由和上游调用
- 预算账本本身（属于 accounting）

## 对外接口

任务 2.1–2.3（#18–#20）中设计。

公开头文件放在 `include/llmgw/middleware/`，其他模块只能使用这里的内容。

## 依赖

`core`、`config`、`observability`、`accounting`（与 [tools/module_deps.json](../../tools/module_deps.json) 一致）

## 线程模型

插件实例每个 Worker 一份，不加锁；跨 Worker 共享的数据只能通过全局对象。

## 相关任务

2.1 (#18)、2.2 (#19)、2.3 (#20)
