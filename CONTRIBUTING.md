# 参与贡献

**中文** | [English summary](#english-summary)

感谢你愿意参与 llmgw。这份文档说明怎样搭建开发环境、怎样提交改动，以及一个改动要满足什么才会被合并。

- 使用问题、想法讨论：[Discussions](https://github.com/bluesky-rgb/llmgw/discussions)
- 缺陷报告、功能请求：[Issues](https://github.com/bluesky-rgb/llmgw/issues/new/choose)
- 安全问题：**不要公开提交**，按 [SECURITY.md](SECURITY.md) 私下报告
- 所有参与者都要遵守 [行为准则](CODE_OF_CONDUCT.md)

## 1. 开发环境

| 需要 | 版本 |
|---|---|
| 操作系统 | Linux x86_64（CI 使用 Ubuntu 24.04） |
| 编译器 | GCC 13 或 Clang 17 及以上（C++20 协程；CI 的静态检查用 Clang 18） |
| 构建 | CMake ≥ 3.24、Ninja |
| 库 | OpenSSL 3（`libssl-dev`）；其余依赖（Boost、simdjson、GoogleTest 等）由 CMake 按 `cmake/dependencies.json` 自动下载 |
| 检查工具 | `clang-format-18`、`clang-tidy-18`、Python 3 |
| 测试工具 | Go（`tools/mock-upstream` 和压测工具，版本见 `tools/mock-upstream/go.mod`） |

Ubuntu 24.04 上：

```bash
sudo apt-get install -y build-essential cmake ninja-build libssl-dev clang-18 clang-format-18 clang-tidy-18 python3 golang-go
```

## 2. 构建与测试

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

第一次配置会下载第三方源码，可以用 `-DFETCHCONTENT_BASE_DIR=$PWD/.deps` 把它们缓存在固定位置。

带检测器的构建（CI 都会运行，涉及并发或内存的改动请在本地先跑）：

```bash
# AddressSanitizer + UndefinedBehaviorSanitizer
cmake -S . -B build-asan -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined"
# ThreadSanitizer
cmake -S . -B build-tsan -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_FLAGS="-fsanitize=thread"
```

## 3. 提交前的检查

CI 的 `architecture` 任务会运行下面这些检查，提交前请在本地先跑一遍：

```bash
# 格式
clang-format-18 --dry-run --Werror $(find src tests/unit tests/integration tests/fuzz tests/common -name '*.cpp' -o -name '*.h' -o -name '*.hpp')
# 模块依赖规则（tools/module_deps.json）
python3 tools/check_deps.py
# TODO 的格式（必须关联 Issue）
python3 tools/check_todos.py
# 静态检查（需要用 Clang 配置一个构建目录）
cmake -S . -B build-tidy -G Ninja -DCMAKE_CXX_COMPILER=clang++-18 -DLLMGW_BUILD_PROTOTYPE=OFF
CLANG_TIDY=clang-tidy-18 tools/run_clang_tidy.sh build-tidy
```

自动修正格式：`clang-format-18 -i <文件>`。

## 4. 开发流程

本项目对每个任务都执行同样的六个步骤，详见 [docs/development-process.md](docs/development-process.md)：

1. **任务设计**：按 [模板](docs/templates/TASK_DESIGN.md) 写清目标与范围、在架构中的位置、接口、失败情况、**验证计划**。大任务放在 `docs/tasks/`，小任务写在 Issue 里
2. **设计评审**：设计通过之后再开工
3. **实现**：核心逻辑先写测试；每个 PR 的改动尽量不超过 400 行
4. **验证**：CI 全部通过，并按验证计划逐项执行、记录结果
5. **合并**：按 [PR 模板](.github/pull_request_template.md) 的检查清单自查
6. **复盘**：更新受影响的设计文档，写下收获与教训

对外部贡献者的实际要求：

| 改动 | 需要 |
|---|---|
| 修正错字、文档、明显的小缺陷 | 直接提 PR |
| 修复缺陷 | 先有 Issue；PR 中包含**能复现问题的测试**（修复前失败、修复后通过） |
| 新功能、改动接口或模块边界 | 先在 Issue 或 Discussions 中讨论，确认方向和设计后再写代码 |

写代码之前，请先读 [docs/code-architecture.md](docs/code-architecture.md)（模块划分与依赖方向）和 [docs/design.md](docs/design.md)（总体设计）。几条最容易违反的规则：

- 模块之间只通过 `include/llmgw/<模块>/` 下的公开头文件交互；新增依赖要更新 `tools/module_deps.json` 并说明理由
- 错误通过返回值（`Result<T>`）传递，热路径不抛异常
- 每个队列、缓存、连接、超时都要有上限
- **日志、错误信息、指标中不能出现 API key、代理密码或请求内容**

## 5. 提交规范

- 提交信息用中文或英文都可以；第一行概括改了什么（不超过 72 个字符），空一行后写为什么这样改
- 一个提交只做一件事；格式调整和逻辑改动分开提交
- PR 描述中关联 Issue（`Fixes #123`）

### DCO（开发者原创声明）

来自 fork 的 PR，每个提交都要带 `Signed-off-by`，表示你同意 [Developer Certificate of Origin 1.1](https://developercertificate.org/)：你有权以本项目的许可证（[Apache 2.0](LICENSE)）提交这些代码。用 `-s` 提交即可：

```bash
git commit -s -m "说明"
# 补签已有的提交（upstream 指向本仓库）
git rebase --signoff upstream/main
```

`Signed-off-by` 中的邮箱必须与提交作者的邮箱（`git config user.email`）一致，CI 的 `dco` 任务会检查。

**关于维护者的提交：** 本仓库的大部分代码由维护者使用 AI 编程助手完成，这些提交的 git 身份是 `Claude <noreply@anthropic.com>`，并带有 `Co-Authored-By` 说明。由 AI 助手声明 DCO 没有意义，所以 DCO 检查只针对来自 fork 的 PR；维护者的提交由维护者负责，以 Apache 2.0 发布。（准确地说，检查跳过的是本仓库内分支发起的 PR，只有维护者能创建这样的分支。）

---

## English summary

The documentation of this project is written in Chinese; issues and pull requests in English are equally welcome.

- **Questions** → [Discussions](https://github.com/bluesky-rgb/llmgw/discussions). **Bugs / feature requests** → [Issues](https://github.com/bluesky-rgb/llmgw/issues/new/choose). **Security issues** → privately, see [SECURITY.md](SECURITY.md).
- **Build:** Linux x86_64, GCC 13 or Clang 17+, CMake ≥ 3.24, Ninja, OpenSSL 3. `cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build && ctest --test-dir build --output-on-failure`.
- **Checks before pushing:** the commands in [section 3](#3-提交前的检查) (clang-format-18, `tools/check_deps.py`, `tools/check_todos.py`, clang-tidy-18).
- **Process:** small fixes → open a PR directly. Bug fixes → include a test that reproduces the bug. New features or interface changes → discuss in an issue first.
- **Rules that matter most:** modules talk only through public headers under `include/llmgw/<module>/`; errors are returned (`Result<T>`), not thrown, on hot paths; every queue, cache and timeout is bounded; never log API keys, proxy passwords or request content.
- **DCO:** every commit in a PR from a fork needs `Signed-off-by` (`git commit -s`) with the same email as the commit author. Maintainer commits are authored with an AI coding assistant and are exempt from the check; the maintainer is responsible for them.
- Everyone follows the [Code of Conduct](CODE_OF_CONDUCT.md).
