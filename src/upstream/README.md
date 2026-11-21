# upstream

## 职责

到上游的 HTTP/1.1 客户端：每个 Worker 的连接池、TLS、出站代理、DNS 缓存。

## 不负责什么

- 重试和切换（属于 executor）
- 请求格式改写（属于 provider）
- SSE 解析（属于 stream）

## 对外接口

任务 1.4（#11）中设计。

公开头文件放在 `include/llmgw/upstream/`，其他模块只能使用这里的内容。

## 依赖

`core`、`config`、`observability`（与 [tools/module_deps.json](../../tools/module_deps.json) 一致）

## 线程模型

连接池每个 Worker 一份，不加锁；DNS 解析在 BlockingPool 中执行。

## 相关任务

1.4 (#11)
