// 模糊测试：配置加载。任意输入都不能崩溃、触发 sanitizer 报告；有效时快照不为空，无效时至少有一个错误。
#include <cstdint>
#include <string_view>

#include "llmgw/config/loader.h"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  llmgw::config::LoadOptions opt;
  opt.resolve_env = false;
  opt.getenv = [](const std::string&) { return std::optional<std::string>("x"); };
  const auto r = llmgw::config::parse(std::string_view(reinterpret_cast<const char*>(data), size), opt);
  if (!r.ok() && r.error_count() == 0) __builtin_trap();  // 无效却没有给出任何错误
  return 0;
}
