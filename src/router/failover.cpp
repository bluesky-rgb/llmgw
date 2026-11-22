// 内置的路由策略：按配置中的顺序（design.md 6.1“依次切换”）。
#include <algorithm>
#include <string>
#include <string_view>

#include "llmgw/router/router.h"

namespace llmgw::router {
namespace {

// 模型名来自客户端，长度、字符都没有限制：只有像配置中的模型名（字母、数字和 "._:/-"，
// 不超过 128 字节）时才放进错误信息，避免把任意内容（几 MB、控制字符）回显给客户端
bool echoable(std::string_view name) {
  constexpr std::size_t kMax = 128;
  return !name.empty() && name.size() <= kMax && std::ranges::all_of(name, [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' ||
           c == '_' || c == ':' || c == '/' || c == '-';
  });
}

class Failover final : public RoutingStrategy {
 public:
  Result<Route> route(const RequestContext& ctx) override {
    const config::ModelConfig* model = ctx.config->find_model(ctx.request.model);
    if (model == nullptr) {
      const std::string what =
          echoable(ctx.request.model) ? "模型 \"" + ctx.request.model + "\"" : "请求的模型";
      return fail(ErrorCode::kModelNotFound, what + "不存在；可用的模型见 GET /v1/models");
    }
    Route r{.model = model, .targets = {}};
    r.targets.reserve(model->targets.size());
    for (const auto& t : model->targets) r.targets.push_back(&t);
    return r;
  }
};

}  // namespace

std::unique_ptr<RoutingStrategy> make_failover() { return std::make_unique<Failover>(); }

}  // namespace llmgw::router
