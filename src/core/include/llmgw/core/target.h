// Target：路由结果中的一个上游目标。
#pragma once

#include <string>

namespace llmgw {

/// 路由器给出的一个候选目标：发给哪个上游、用哪个模型名。
///
/// 只保存名字；上游的地址、超时、改写规则等从配置快照中按 provider 查找，
/// 这样 Target 很小，可以放心复制，也不会和配置快照的生命周期纠缠。
struct Target {
  std::string provider;  ///< 配置中的上游名，比如 "openai"
  std::string model;     ///< 发给上游的模型名，比如 "gpt-5.6-sol"

  friend bool operator==(const Target&, const Target&) = default;
};

}  // namespace llmgw
