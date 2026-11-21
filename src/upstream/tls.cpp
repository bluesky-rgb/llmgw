#include "tls.h"

#include <openssl/ssl.h>

namespace llmgw::upstream {

namespace ssl = boost::asio::ssl;

ssl::context* TlsContexts::find(const std::string& ca_file) const {
  auto it = contexts_.find(ca_file);
  return it == contexts_.end() ? nullptr : it->second.get();
}

Result<std::shared_ptr<const TlsContexts>> make_tls_contexts(const config::Snapshot& snapshot) {
  auto out = std::make_shared<TlsContexts>();
  for (const auto& p : snapshot.providers) {
    if (p.base_url.scheme != "https" || out->contexts_.contains(p.ca_file)) continue;
    auto ctx = std::make_unique<ssl::context>(ssl::context::tls_client);
    // 最低 TLS 1.2（design.md 5.3）；校验证书链，主机名在每条连接上单独设置
    SSL_CTX_set_min_proto_version(ctx->native_handle(), TLS1_2_VERSION);
    ctx->set_options(ssl::context::default_workarounds | ssl::context::no_compression);
    ctx->set_verify_mode(ssl::verify_peer);
    boost::system::error_code ec;
    if (p.ca_file.empty()) {
      ctx->set_default_verify_paths(ec);
      if (ec) return fail(ErrorCode::kInternal, "无法加载系统证书库：" + ec.message());
    } else {
      ctx->load_verify_file(p.ca_file, ec);
      if (ec) {
        return fail(ErrorCode::kInternal, "上游 \"" + p.name + "\" 的 ca_file 无法加载（" +
                                              p.ca_file + "）：" + ec.message());
      }
    }
    out->contexts_.emplace(p.ca_file, std::move(ctx));
  }
  return std::shared_ptr<const TlsContexts>(std::move(out));
}

}  // namespace llmgw::upstream
