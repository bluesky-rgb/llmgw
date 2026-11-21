# core

## 职责

所有模块共用的基础类型与接口，以及版本信息。只有头文件，没有运行时状态。

## 不负责什么

- 任何具体实现和 I/O
- 配置快照的内容（config 模块）、中间件的放行/拒绝结果 `Decision`（middleware 模块）
- 错误的 JSON 序列化（server 模块）

## 对外接口

公开头文件放在 `include/llmgw/core/`，其他模块只能使用这里的内容。每个头文件都能单独编译（`llmgw_add_module` 自动检查）。

| 头文件 | 内容 |
|---|---|
| `task.h` | `Task<T>`：异步函数的返回类型 |
| `result.h` | `Result<T>`、`fail()`：可能失败的操作的返回类型 |
| `error.h` | `ErrorCode`、`Error`；`http_status()`、`code_name()`、`should_retry()`（与 [client-guide.md 第 7 节](../../docs/client-guide.md#7-错误处理与重试)一一对应） |
| `target.h` | `Target`：路由结果中的一个上游目标 |
| `response.h` | `ResponseHead`、`Chunk`、`ResponseStream`（Session 拉取响应体的接口） |
| `request_context.h` | `RequestContext`：按写入者分组的请求数据；`Attempt`、`Usage`、`Outcome` |
| `metrics.h` | `MetricsRecorder`（实现在 observability）、指标目录、`NullMetricsRecorder` |
| `version.h` | 版本号（由 CMake 的项目版本生成） |

设计与类图见 [docs/tasks/0.4-core-interfaces.md](../../docs/tasks/0.4-core-interfaces.md)。

## 依赖

- 模块依赖：无（与 [tools/module_deps.json](../../tools/module_deps.json) 一致）
- 第三方（[ADR-007](../../docs/decisions.md#adr-007core-的两个第三方依赖tlexpected-与-asio-的-awaitable)）：
  - **Boost.Asio**：只使用 `awaitable`，集中在 `task.h`
  - **tl::expected**：`Result<T>` 的实现，集中在 `result.h`

其他头文件只使用 `Task` 和 `Result`，不直接出现 `boost::asio::` 或 `tl::`。

## 使用约定

- 协程参数不用引用；调用方保证存活的对象（如 `RequestContext`）传指针（[code-architecture.md 3.4](../../docs/code-architecture.md#34-协程专项规则)）
- `RequestContext` 的每组字段只有一个写入者，写在字段旁边；`request` 和 `config` 是 const 成员
- `MetricsRecorder` 不要在每个数据块上调用；标签值个数必须与目录登记的一致，否则这次上报被丢弃并计入 `dropped()`
- 新增错误码：修改 `ErrorCode` 和三个映射函数（漏写会编译失败），同步 client-guide.md 和 `tests/unit/core/error_test.cpp`
- 新增指标：在枚举和对应的目录数组中各加一行（顺序不一致会编译失败）

## 线程模型

只有类型定义，没有运行时状态。`ResponseStream`、`MetricsRecorder` 的实例只在创建它们的 Worker 线程上使用。

## 相关任务

0.4 (#4)
