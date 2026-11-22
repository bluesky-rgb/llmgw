// 路由（任务 1.7 X17）：顺序与配置一致；模型不存在时 404，不回显任意内容。
#include "llmgw/router/router.h"

#include <gtest/gtest.h>

#include "llmgw/config/snapshot.h"

namespace llmgw::router {
namespace {

std::shared_ptr<config::Snapshot> snapshot() {
  auto s = std::make_shared<config::Snapshot>();
  config::ModelConfig m;
  m.name = "smart";
  for (const char* p : {"openai", "minimax", "mimo"}) {
    config::TargetConfig t;
    t.provider = p;
    t.model = std::string(p) + "-model";
    m.targets.push_back(t);
  }
  s->models.push_back(m);
  return s;
}

Result<Route> route(const std::shared_ptr<config::Snapshot>& s, std::string model) {
  RequestInfo info;
  info.model = std::move(model);
  RequestContext ctx(std::move(info), s);
  return make_failover()->route(ctx);
}

TEST(Router, X17_failover_keeps_config_order) {
  auto s = snapshot();
  auto r = route(s, "smart");
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(r->model, &s->models[0]);
  ASSERT_EQ(r->targets.size(), 3U);
  for (std::size_t i = 0; i < 3; ++i)
    EXPECT_EQ(r->targets[i], &s->models[0].targets[i]) << "指向快照，不复制";
}

TEST(Router, X17_unknown_model_is_404) {
  auto s = snapshot();
  auto r = route(s, "gpt-9");
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error().code, ErrorCode::kModelNotFound);
  EXPECT_NE(r.error().message.find("\"gpt-9\""), std::string::npos);
  // 大小写敏感：与配置的模型名完全一致才匹配
  EXPECT_FALSE(route(s, "Smart").has_value());
}

TEST(Router, X17_unknown_model_name_is_echoed_only_when_harmless) {
  auto s = snapshot();
  for (const std::string& bad :
       {std::string(129, 'a'), std::string("a\nb"), std::string("<script>"), std::string("a\"b")}) {
    auto r = route(s, bad);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().message.find(bad), std::string::npos) << "不回显：" << bad.substr(0, 10);
    EXPECT_EQ(r.error().code, ErrorCode::kModelNotFound);
  }
}

}  // namespace
}  // namespace llmgw::router
