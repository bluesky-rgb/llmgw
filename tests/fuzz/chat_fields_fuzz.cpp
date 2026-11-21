// 模糊测试：从请求体中取 model、stream（simdjson on-demand）。成功时 model 必须非空。
#include <cstdint>
#include <string>

#include "http_util.h"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  std::string body(reinterpret_cast<const char*>(data), size);
  auto r = llmgw::server::detail::parse_chat_fields(&body);
  if (r && r->model.empty()) __builtin_trap();
  std::mt19937_64 rng(1);
  (void)llmgw::server::detail::request_id(std::string_view(reinterpret_cast<const char*>(data), size), rng);
  (void)llmgw::server::detail::path_of(std::string_view(reinterpret_cast<const char*>(data), size));
  return 0;
}
