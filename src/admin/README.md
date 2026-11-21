# admin

## 职责

管理面接口：重新加载配置、状态、用量、日志级别；审计日志。

## 不负责什么

- 直接修改其他模块的状态（只能通过 config 提交新配置）

## 对外接口

任务 3.4（#30）中设计。

公开头文件放在 `include/llmgw/admin/`，其他模块只能使用这里的内容。

## 依赖

`core`、`config`、`observability`、`accounting`（与 [tools/module_deps.json](../../tools/module_deps.json) 一致）

## 线程模型

运行在管理端口上，默认只监听 127.0.0.1。

## 相关任务

3.4 (#30)
