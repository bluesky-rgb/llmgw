// Gateway：server::Dispatcher 的实现 = router + executor（任务 1.7）。
// app 是唯一同时看得到 server、router、executor、upstream 的模块，所以在这里组装。
#pragma once

#include <memory>

#include "llmgw/config/snapshot.h"
#include "llmgw/server/server.h"
#include "llmgw/upstream/client.h"

namespace llmgw::app {

/// 为每个 Worker 创建一个 Gateway：自己的 upstream::Client（连接池）、路由策略、Executor。
/// config、tls 被所有 Worker 共享（只读）。
server::DispatcherFactory make_dispatcher_factory(std::shared_ptr<const config::Snapshot> config,
                                                  std::shared_ptr<const upstream::TlsContexts> tls);

}  // namespace llmgw::app
