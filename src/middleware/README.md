# middleware

## 职责

中间件接口与中间件链、插件注册表；内置鉴权、限流、预算三个插件。

## 不负责什么

- 路由和上游调用
- 预算账本本身（属于 accounting）

## 对外接口

公开头文件放在 `include/llmgw/middleware/`，其他模块只能使用这里的内容（设计见 [docs/tasks/2.1-middleware-chain.md](../../docs/tasks/2.1-middleware-chain.md)）。

| 头文件 | 内容 |
|---|---|
| `middleware.h` | `Middleware`（`on_request` / `on_complete`）、`Decision`（放行 / 拒绝 + 附加响应头）、`MiddlewareFactory`、`PluginSpec`、`WorkerResources` |
| `registry.h` | `Registry`（启动时登记，之后只读）、`register_builtin()`（ADR-009：显式登记）、`validate_pipeline()` |
| `chain.h` | `Chain`：每个 Worker 一条；`on_request` 按顺序、遇到拒绝停下；`on_complete` 按相反的顺序、只对进入过的插件；插件的异常在这里捕获 |

**保证**：只要某个插件的 `on_request` 被调用过，它的 `on_complete` 就一定会被调用一次（被拒绝、抛异常、客户端断开、超时都一样）。由 Session 实现：`on_complete` 在竞速结束之后执行，不会被客户端断开取消；整个阶段限时 5 秒。两个例外：限时到了时，剩下的插件不再执行；进程退出时还在进行的请求。

**生命周期**：插件实例先于事件循环销毁，还没有结束的插件协程帧在那之后才销毁：协程帧中的局部对象析构时、插件启动的后台任务，都不能访问插件对象本身（与 Dispatcher 的规则相同）。

## 依赖

目前只用到 `core`、`config`；规则允许 `core`、`config`、`observability`、`accounting`（[tools/module_deps.json](../../tools/module_deps.json)），预算插件（2.4）会用到 accounting。

## 线程模型

插件实例每个 Worker 一份，不加锁；跨 Worker 共享的数据只能通过全局对象。

## 相关任务

2.1 (#18)、2.2 (#19)、2.3 (#20)
