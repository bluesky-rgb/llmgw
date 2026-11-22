# app

## 职责

程序入口和命令行（serve、check、keygen、reload、status、usage、chat），负责组装所有模块。

## 不负责什么

- 任何业务逻辑

## 对外接口

`llmgw` 可执行文件（`build/src/app/llmgw`）：

| 命令 | 状态 |
|---|---|
| `llmgw check [-c 配置文件] [--skip-env]` | ✅ 校验配置文件（任务 1.1）。退出码：0 有效，1 有错误，2 参数错误或文件读不了 |
| `llmgw serve [-c 配置文件]` | ✅ 启动网关（任务 1.7）。启动后在标准错误输出实际监听的地址；SIGINT / SIGTERM 时停止（进行中的请求会被中断，优雅退出见 3.3）。退出码：0 正常停止，1 配置错误或启动失败，2 参数错误或文件读不了 |
| `llmgw version` | ✅ |
| `keygen`、`reload`、`status`、`usage`、`chat` | 任务 2.2、3.4（#30） |

内部结构：`gateway.cpp`（`server::Dispatcher` 的实现 = router + executor，每个 Worker 一个，各自持有一个 `upstream::Client`）、`serve.cpp`。两者编译成静态库 `llmgw_app_lib`，可执行文件和测试都链接它。

## 依赖

目前：`core`、`config`、`upstream`、`router`、`executor`、`server`。最终：`core`、`config`、`observability`、`upstream`、`stream`、`accounting`、`router`、`provider`、`middleware`、`executor`、`server`、`admin`（与 [tools/module_deps.json](../../tools/module_deps.json) 一致）

## 线程模型

主线程：加载配置、创建 TLS 上下文、屏蔽 SIGINT / SIGTERM（在创建任何线程之前，新线程继承信号掩码），然后 `sigwait`。请求都在 Server 的 Worker 线程上处理。

## 相关任务

1.7 (#14)、1.9 (#16)、3.4 (#30)
