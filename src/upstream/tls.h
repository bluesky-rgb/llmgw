// 全局共享的 TLS 上下文。启动时创建（加载系统证书库约 40 ms），之后只读。
// OpenSSL 保证配置完成、不再修改的 SSL_CTX 可以在多个线程上同时用来创建连接。
#pragma once

#include <boost/asio/ssl/context.hpp>
#include <map>
#include <memory>
#include <string>

#include "llmgw/upstream/client.h"

namespace llmgw::upstream {

class TlsContexts {
 public:
  /// ca_file 为空表示系统证书库。找不到时返回 nullptr（配置与 make_tls_contexts 的输入不一致）
  [[nodiscard]] boost::asio::ssl::context* find(const std::string& ca_file) const;

 private:
  friend Result<std::shared_ptr<const TlsContexts>> make_tls_contexts(const config::Snapshot&);
  // SSL_CTX 内部有引用计数和锁，创建连接时需要非 const 的指针；内容在启动后不再修改
  std::map<std::string, std::unique_ptr<boost::asio::ssl::context>> contexts_;
};

}  // namespace llmgw::upstream
