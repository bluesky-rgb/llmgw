#include "tls_certs.h"

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>
#include <unistd.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>

namespace llmgw::test {
namespace {

struct KeyFree {
  void operator()(EVP_PKEY* k) const { EVP_PKEY_free(k); }
};
struct CertFree {
  void operator()(X509* c) const { X509_free(c); }
};
using Key = std::unique_ptr<EVP_PKEY, KeyFree>;
using Cert = std::unique_ptr<X509, CertFree>;

void check(bool ok, const char* what) {
  if (!ok) throw std::runtime_error(std::string("生成测试证书失败：") + what);
}

Key new_key() {
  Key k(EVP_EC_gen("P-256"));
  check(k != nullptr, "EVP_EC_gen");
  return k;
}

void add_ext(X509* cert, X509* issuer, int nid, const char* value) {
  X509V3_CTX ctx;
  X509V3_set_ctx_nodb(&ctx);
  X509V3_set_ctx(&ctx, issuer, cert, nullptr, nullptr, 0);
  X509_EXTENSION* ext = X509V3_EXT_conf_nid(nullptr, &ctx, nid, value);
  check(ext != nullptr, "X509V3_EXT_conf_nid");
  X509_add_ext(cert, ext, -1);
  X509_EXTENSION_free(ext);
}

// 签发证书：issuer 为空表示自签名。days_from/days_to 是相对现在的有效期（天）
Cert make_cert(EVP_PKEY* key, const char* cn, const char* san, X509* issuer, EVP_PKEY* issuer_key,
               long days_from, long days_to, bool is_ca) {
  static long serial = 1;
  Cert c(X509_new());
  X509_set_version(c.get(), 2);
  ASN1_INTEGER_set(X509_get_serialNumber(c.get()), serial++);
  X509_gmtime_adj(X509_getm_notBefore(c.get()), days_from * 86400);
  X509_gmtime_adj(X509_getm_notAfter(c.get()), days_to * 86400);
  X509_set_pubkey(c.get(), key);
  X509_NAME* name = X509_get_subject_name(c.get());
  X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC, reinterpret_cast<const unsigned char*>(cn),
                             -1, -1, 0);
  X509* signer = issuer != nullptr ? issuer : c.get();
  X509_set_issuer_name(c.get(), X509_get_subject_name(signer));
  if (is_ca) {
    add_ext(c.get(), signer, NID_basic_constraints, "critical,CA:TRUE");
    add_ext(c.get(), signer, NID_key_usage, "critical,keyCertSign,cRLSign");
  } else {
    add_ext(c.get(), signer, NID_basic_constraints, "CA:FALSE");
    add_ext(c.get(), signer, NID_subject_alt_name, san);
    add_ext(c.get(), signer, NID_ext_key_usage, "serverAuth");
  }
  check(X509_sign(c.get(), issuer_key != nullptr ? issuer_key : key, EVP_sha256()) > 0,
        "X509_sign");
  return c;
}

std::string write_file(const std::filesystem::path& dir, const std::string& name,
                       const std::string& text) {
  const auto path = dir / name;
  std::ofstream(path) << text;
  return path.string();
}

std::string pem(X509* c) {
  BIO* b = BIO_new(BIO_s_mem());
  PEM_write_bio_X509(b, c);
  char* data = nullptr;
  const long n = BIO_get_mem_data(b, &data);
  std::string out(data, static_cast<std::size_t>(n));
  BIO_free(b);
  return out;
}

std::string pem(EVP_PKEY* k) {
  BIO* b = BIO_new(BIO_s_mem());
  PEM_write_bio_PrivateKey(b, k, nullptr, nullptr, 0, nullptr, nullptr);
  char* data = nullptr;
  const long n = BIO_get_mem_data(b, &data);
  std::string out(data, static_cast<std::size_t>(n));
  BIO_free(b);
  return out;
}

TestPki generate() {
  const auto dir =
      std::filesystem::temp_directory_path() / ("llmgw-pki-" + std::to_string(::getpid()));
  std::filesystem::create_directories(dir);
  Key ca_key = new_key();
  Cert ca = make_cert(ca_key.get(), "llmgw test CA", nullptr, nullptr, nullptr, -1, 3650, true);
  TestPki pki;
  pki.ca_file = write_file(dir, "ca.pem", pem(ca.get()));
  auto leaf = [&](const char* cn, const char* san, long from, long to, bool self) {
    Key k = new_key();
    Cert c = self ? make_cert(k.get(), cn, san, nullptr, nullptr, from, to, false)
                  : make_cert(k.get(), cn, san, ca.get(), ca_key.get(), from, to, false);
    return CertFiles{pem(c.get()), pem(k.get())};
  };
  pki.good = leaf("localhost", "DNS:localhost,IP:127.0.0.1", -1, 365, false);
  pki.wrong_host = leaf("other.example", "DNS:other.example", -1, 365, false);
  pki.expired = leaf("localhost", "DNS:localhost,IP:127.0.0.1", -3, -1, false);
  pki.self_signed = leaf("localhost", "DNS:localhost,IP:127.0.0.1", -1, 365, true);
  return pki;
}

}  // namespace

const TestPki& test_pki() {
  static const TestPki pki = generate();
  return pki;
}

}  // namespace llmgw::test
