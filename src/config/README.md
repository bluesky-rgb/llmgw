# config

## 职责

把配置文件读成一份不可变的 `config::Snapshot`，并校验它。配置有问题时**一次列出所有问题**，每个问题指出 JSON 路径，拼错的名字给出“是不是 xxx”的建议。

## 不负责什么

- 热加载、`ConfigStore`（任务 4.x）
- 使用配置：路由、改写、超时由各自的模块读取快照

## 对外接口

| 头文件 | 内容 |
|---|---|
| `snapshot.h` | `Snapshot` 及各部分的类型；`find_provider()`、`find_model()` |
| `loader.h` | `parse()`、`load_file()` → `LoadReport`（快照 + 全部问题）；`parse_field_path()`、`parse_url()`、`parse_endpoint()` |

支持的字段见示例 [config/llmgw.example.json](../../config/llmgw.example.json)，校验规则见[任务设计 3.2](../../docs/tasks/1.1-config.md#32-校验规则)。

**未知字段、尚未支持的部分（`keys`、`pricing`、`accounting`、`retry`、`providers[].quota`）都会报错**，而不是被忽略：安全相关的配置不能静默失效。

## 内部结构

| 文件 | 内容 |
|---|---|
| `json_reader.h/.cpp` | 带 JSON 路径的类型化读取；读完一个对象后报告未知字段；拼写建议 |
| `sections.h/.cpp` | 各部分的解析：server、timeouts、providers、proxy、models |
| `loader.cpp` | 加载流程；跨部分的检查（引用关系、重名、未使用的上游）；读取环境变量中的密钥 |
| `syntax.cpp` | 字段路径、URL、监听地址的语法 |

nlohmann/json 只在这些 `.cpp` 中使用，不出现在公开头文件中。

## 依赖

- 模块：`core`（与 [tools/module_deps.json](../../tools/module_deps.json) 一致）
- 第三方：nlohmann/json（私有）

## 线程模型

只在启动（以及以后的热加载）时调用，不在热路径上。读取环境变量只在加载时进行；快照创建后只读，可以在所有 Worker 之间共享。

## 已知限制

- 同一个对象中重复的键：nlohmann/json 保留最后一个，不报错
- `proxy.https` 为 `"env"` 时只读取 `HTTPS_PROXY` / `https_proxy`，不支持 `NO_PROXY`

## 相关任务

1.1 (#8)
