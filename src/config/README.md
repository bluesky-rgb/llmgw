# config

## 职责

解析和校验配置；`ConfigStore` 保存不可变的配置快照，热加载时整体原子替换。

## 不负责什么

- 配置更新后各 Worker 的状态迁移（由各 Worker 自己处理，见 design.md 5.5）
- 密钥管理（上游 key 只从环境变量读取）

## 对外接口

任务 1.1（#8）、3.1（#27）中设计。

公开头文件放在 `include/llmgw/config/`，其他模块只能使用这里的内容。

## 依赖

`core`（与 [tools/module_deps.json](../../tools/module_deps.json) 一致）

## 线程模型

`ConfigStore` 全局一个，原子替换；快照本身只读，可以跨线程共享。

## 相关任务

1.1 (#8)、3.1 (#27)
