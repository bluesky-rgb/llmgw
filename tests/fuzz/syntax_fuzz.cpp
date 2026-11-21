// 模糊测试：URL、代理地址、字段路径、监听地址。解析成功的字段路径转回文本后，必须能再次解析成同样的结果。
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string>
#include <string_view>

#include "llmgw/config/loader.h"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  const std::string_view text(reinterpret_cast<const char*>(data), size);
  (void)llmgw::config::parse_url(text);
  (void)llmgw::config::parse_endpoint(text);
  // 代理地址（任务 1.4）：错误信息中不能出现密码。取 "://用户名:密码@" 中的密码，
  // 只检查至少 6 位的纯字母数字密码：固定的错误信息里最长的字母数字片段是 "HTTPS"（5 位），
  // 否则输入恰好是错误信息的子串时会误报（第一次运行就遇到了：输入 "以 http:// 开"）
  if (auto proxy = llmgw::config::parse_proxy_url(text); !proxy) {
    const auto scheme = text.find("://");
    const auto at = text.rfind('@');
    if (scheme != std::string_view::npos && at != std::string_view::npos && at > scheme) {
      const auto userinfo = text.substr(scheme + 3, at - scheme - 3);
      const auto colon = userinfo.find(':');
      const auto password =
          colon == std::string_view::npos ? std::string_view{} : userinfo.substr(colon + 1);
      const bool alnum = std::all_of(password.begin(), password.end(), [](char c) {
        return std::isalnum(static_cast<unsigned char>(c)) != 0;
      });
      if (password.size() >= 6 && alnum &&
          proxy.error().message.find(password) != std::string::npos) {
        __builtin_trap();
      }
    }
  }
  // 任何 NO_PROXY
  // 项都匹配它自己。全由点组成的项（"."、".."）去掉结尾的点后为空，不匹配任何主机，除外
  const bool only_dots = text.find_first_not_of('.') == std::string_view::npos;
  if (!only_dots && text != "*" && !llmgw::config::no_proxy_matches(text, text)) __builtin_trap();
  if (auto p = llmgw::config::parse_field_path(text)) {
    auto again = llmgw::config::parse_field_path(llmgw::config::to_string(*p));
    if (!again || *again != *p) __builtin_trap();
  }
  return 0;
}
