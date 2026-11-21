// 测试用的证书：启动时用 OpenSSL 生成，写到临时目录。仓库里不保存任何私钥。
#pragma once

#include <string>

namespace llmgw::test {

struct CertFiles {
  std::string pem_cert;  ///< 证书（PEM）
  std::string pem_key;   ///< 私钥（PEM）
};

struct TestPki {
  std::string ca_file;    ///< CA 证书的文件路径（给 providers[].ca_file）
  CertFiles good;         ///< localhost、127.0.0.1，由 CA 签发
  CertFiles wrong_host;   ///< other.example，由 CA 签发
  CertFiles expired;      ///< localhost，由 CA 签发，已过期
  CertFiles self_signed;  ///< localhost，自签名（不是由 CA 签发）
};

/// 生成一套证书（每个进程一次，之后复用）。
const TestPki& test_pki();

}  // namespace llmgw::test
