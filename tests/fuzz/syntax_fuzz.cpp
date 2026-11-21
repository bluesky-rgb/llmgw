// 模糊测试：URL、字段路径、监听地址。解析成功的字段路径转回文本后，必须能再次解析成同样的结果。
#include <cstdint>
#include <string_view>

#include "llmgw/config/loader.h"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  const std::string_view text(reinterpret_cast<const char*>(data), size);
  (void)llmgw::config::parse_url(text);
  (void)llmgw::config::parse_endpoint(text);
  if (auto p = llmgw::config::parse_field_path(text)) {
    auto again = llmgw::config::parse_field_path(llmgw::config::to_string(*p));
    if (!again || *again != *p) __builtin_trap();
  }
  return 0;
}
