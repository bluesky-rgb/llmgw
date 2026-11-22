// llmgw serve：加载配置，启动网关，收到 SIGINT / SIGTERM 时停止（任务 1.7）。
#pragma once

#include <string_view>
#include <vector>

namespace llmgw::app {

/// 退出码：0 收到信号后正常停止；1 配置错误或启动失败（端口被占用 …）；2 参数错误、文件读不了。
int serve(const std::vector<std::string_view>& args);

}  // namespace llmgw::app
