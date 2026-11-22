# router

## 职责

根据对外的模型名得到按尝试顺序排列的目标列表；路由策略扩展点（`RoutingStrategy`）。

## 不负责什么

- 发送请求、重试、熔断（属于 executor）。路由只决定“按什么顺序试”，“试了失败怎么办”由 executor 的重试规则决定，那是核心，不是扩展点

## 对外接口

`include/llmgw/router/router.h`：

```cpp
auto strategy = router::make_failover();   // 每个 Worker 一个
auto route = strategy->route(*ctx);        // Result<Route>；模型不存在时 404 model_not_found
// route->targets：指向 ctx->config 快照中的 TargetConfig，按尝试顺序排列，不为空
```

- 内置的 `make_failover()`：按配置中的顺序
- 模型名区分大小写；客户端给出的模型名只有像配置中的模型名（字母、数字和 `._:/-`，不超过 128 字节）时才放进错误信息

## 依赖

`core`、`config`（与 [tools/module_deps.json](../../tools/module_deps.json) 一致）

## 线程模型

每个 Worker 一个策略实例，只在该 Worker 的线程上调用：以后有状态的策略（轮询、按延迟）不需要加锁。同步接口：路由是纯计算。

## 相关任务

1.7 (#14)，设计见 [docs/tasks/1.7-router-executor.md](../../docs/tasks/1.7-router-executor.md)
