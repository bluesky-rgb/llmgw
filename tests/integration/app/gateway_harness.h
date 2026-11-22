// 进程内的网关（Server + Gateway）与访问它的客户端：X18（gateway_test.cpp）和 1.9 的迁移测试共用。
#pragma once

#include <algorithm>
#include <cctype>
#include <memory>

#include "../upstream/harness.h"
#include "gateway.h"
#include "llmgw/server/server.h"
#include "plugins.h"

namespace llmgw::test {

/// 进程内启动的网关：真实的 Server + 每个 Worker 一个 Gateway（router + executor +
/// upstream::Client）
struct Gateway {
  std::unique_ptr<server::Server> server;
  /// chain：中间件链（任务 2.2 起：auth 等真实的插件，用 app::make_chain_factory 创建）
  explicit Gateway(const std::shared_ptr<config::Snapshot>& cfg, server::ChainFactory chain = {}) {
    auto tls = upstream::make_tls_contexts(*cfg);
    if (!tls) throw std::runtime_error(tls.error().message);
    server = std::make_unique<server::Server>(
        server::ServerOptions{.config = cfg,
                              .dispatcher_factory = app::make_dispatcher_factory(cfg, *tls),
                              .metrics_factory = {},
                              .chain_factory = std::move(chain)});
    if (auto r = server->start(); !r) throw std::runtime_error(r.error().message);
  }
};

// 访问网关的客户端：网关本身当作一个“上游”
struct Caller {
  Env env;
  const config::Provider* gw = nullptr;
  explicit Caller(std::uint16_t port) { gw = env.start(provider("http", "127.0.0.1", port)); }
  Fetched chat(const std::string& body, std::vector<Header> extra = {}) {
    upstream::Request req = post(body);
    req.target = "/v1/chat/completions";
    for (auto& h : extra) req.headers.push_back(std::move(h));
    return env.get(gw, std::move(req), deadlines(2s, 10s));
  }
  /// 用指定的 Authorization 访问（空字符串：不带 Authorization）。target 为空时是聊天接口
  Fetched call_with_auth(const std::string& authorization, const std::string& body,
                         const std::string& method = "POST",
                         const std::string& target = "/v1/chat/completions") {
    upstream::Request req = post(body);
    req.method = method;
    req.target = target;
    std::erase_if(req.headers, [](const Header& h) { return h.name == "Authorization"; });
    if (!authorization.empty()) req.headers.push_back({"Authorization", authorization});
    if (method == "GET") req.body.clear();
    return env.get(gw, std::move(req), deadlines(2s, 10s));
  }
};

inline std::string header(const Fetched& f, std::string_view name) {
  for (const auto& h : f.headers) {
    if (h.name.size() == name.size() &&
        std::equal(h.name.begin(), h.name.end(), name.begin(), [](char x, char y) {
          return std::tolower(static_cast<unsigned char>(x)) ==
                 std::tolower(static_cast<unsigned char>(y));
        }))
      return h.value;
  }
  return {};
}

}  // namespace llmgw::test
