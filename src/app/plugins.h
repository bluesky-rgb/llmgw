// 插件的组装（任务 2.1）：注册表、加载配置时校验 pipeline、为每个 Worker 建中间件链、llmgw
// plugins。 app 是唯一同时看得到 config、middleware、server 的模块，所以在这里组装。
#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "llmgw/config/loader.h"
#include "llmgw/config/snapshot.h"
#include "llmgw/middleware/registry.h"
#include "llmgw/server/server.h"

namespace llmgw::app {

/// 登记了全部内置插件的注册表。
std::shared_ptr<const middleware::Registry> builtin_registry();

/// 加载配置文件，并用 registry 校验 pipeline（插件名、插件自己的配置），问题追加在报告的末尾；
/// 有错误时 snapshot 为空。llmgw check 和 llmgw serve 都用它，保证两者的结论一致。
/// 限制：配置的其他部分有错误时没有快照，插件不会被校验，要改完那些错误之后才能看到插件的问题。
config::LoadReport load_config(const std::string& path, const config::LoadOptions& options,
                               const middleware::Registry& registry);

/// pipeline 中是否启用了 auth 插件（客户端鉴权）。
bool has_auth(const config::Snapshot& snapshot);

/// llmgw keygen --id <id>：生成一个网关 key，输出 key（只显示一次）和要写进配置的哈希。返回退出码。
int keygen(const std::vector<std::string_view>& args);

/// 为每个 Worker 按 snapshot.pipeline 创建中间件链。pipeline 为空时返回空的工厂（不创建链）。
server::ChainFactory make_chain_factory(std::shared_ptr<const config::Snapshot> snapshot,
                                        std::shared_ptr<const middleware::Registry> registry);

/// llmgw plugins：列出注册的插件。返回退出码。
int list_plugins(const middleware::Registry& registry);

}  // namespace llmgw::app
