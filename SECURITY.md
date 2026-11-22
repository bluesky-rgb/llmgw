# 安全策略 / Security Policy

## 支持的版本 / Supported versions

| 版本 | 是否接收安全修复 |
|---|---|
| `main` 分支、最新发布的版本 | 是 |
| 更早的版本 | 否（请升级） |

The `main` branch and the latest release receive security fixes. Older releases do not.

> **当前状态：** 客户端鉴权是可选的（`pipeline` 中的 `auth` 插件，任务 2.2）。没有启用鉴权时默认只允许监听本机地址，监听其他地址需要显式加 `--allow-unauthenticated`。这个检查只针对数据面（`server.host`）；观测面（默认 `0.0.0.0:9100`）目前只提供 `/health`、`/ready`，不含敏感信息，`/metrics` 上线（任务 2.8）之前会把检查扩展到观测面。“没有鉴权的网关被他人访问”属于已知限制，不作为漏洞处理；但绕过这个默认限制的方法是漏洞。
>
> **Current status:** client authentication is optional (the `auth` plugin in `pipeline`, task 2.2). Without it, the gateway refuses to listen on non-loopback addresses unless started with `--allow-unauthenticated`. The check covers the data plane (`server.host`); the observability listener (default `0.0.0.0:9100`) currently serves only `/health` and `/ready`, and will be covered before `/metrics` lands (task 2.8). Access to an unauthenticated gateway is a known limitation, but a way to bypass the loopback-only default *is* a vulnerability.

## 报告漏洞 / Reporting a vulnerability

**请不要在公开的 Issue、PR 或 Discussions 中报告安全问题。**

使用 GitHub 的私密漏洞报告：[Security → Report a vulnerability](https://github.com/bluesky-rgb/llmgw/security/advisories/new)。请写明：

- 影响的版本或提交
- 复现步骤（最小的配置和请求）
- 影响：能做到什么（例如读到 key、让网关崩溃、绕过限制）

**Please do not report security issues in public issues, pull requests or discussions.** Use GitHub private vulnerability reporting: [Security → Report a vulnerability](https://github.com/bluesky-rgb/llmgw/security/advisories/new). Include the affected version, reproduction steps and the impact.

## 响应 / Response

| 阶段 | 时间 |
|---|---|
| 确认收到 | 7 天内 |
| 给出评估，以及修复或缓解方案 | 30 天内 |
| 发布修复 | 修复完成后尽快；同时发布 GitHub Security Advisory，并在报告人同意时致谢 |

项目由一位维护者维护，以上是能做到的承诺。

We acknowledge reports within 7 days and provide an assessment with a fix or mitigation plan within 30 days. Fixes are released with a GitHub Security Advisory; reporters are credited if they wish.

## 范围 / Scope

属于安全问题的例子：

- API key、代理密码或请求内容出现在日志、错误信息、指标或响应中
- 畸形的请求或上游响应导致崩溃、内存错误、无限占用内存或连接
- 绕过“默认只监听本机”的限制；绕过鉴权（auth 插件）；以后的限额被绕过
- 网关 key 出现在日志、错误信息、指标中，或者被转发给上游
- 依赖库中影响 llmgw 的已知漏洞

不属于：上游供应商自身的问题；需要先控制网关所在主机或配置文件才能做到的事；没有鉴权的网关被访问（见上文）。

In scope: secret leakage (keys, proxy passwords, request content) through logs, errors, metrics or responses; crashes, memory errors or unbounded resource use caused by malformed requests or upstream responses; bypassing the loopback-only default, authentication, or (future) quotas; gateway keys appearing in logs, errors or metrics, or forwarded upstream; vulnerable dependencies affecting llmgw. Out of scope: issues of upstream providers; attacks that require control of the host or the config file; access to an unauthenticated gateway.
