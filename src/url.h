#pragma once

#include <string>

namespace llmgw {

// 把 "https://api.openai.com/v1" 拆成 httplib 客户端需要的两部分
struct BaseUrl {
  std::string scheme_host_port;  // "https://api.openai.com:443"
  std::string path_prefix;       // "/v1"（没有路径时为空，且不以 / 结尾）
};

// 格式不正确时抛出 std::invalid_argument
BaseUrl parse_base_url(const std::string& url);

}  // namespace llmgw
