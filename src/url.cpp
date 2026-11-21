#include "url.h"

#include <stdexcept>

namespace llmgw {

BaseUrl parse_base_url(const std::string& url) {
  const auto scheme_end = url.find("://");
  if (scheme_end == std::string::npos) {
    throw std::invalid_argument("缺少 http:// 或 https:// 前缀: " + url);
  }
  const std::string scheme = url.substr(0, scheme_end);
  if (scheme != "http" && scheme != "https") {
    throw std::invalid_argument("只支持 http 和 https: " + url);
  }

  const auto host_begin = scheme_end + 3;
  const auto path_begin = url.find('/', host_begin);
  std::string host_port = url.substr(host_begin, path_begin == std::string::npos
                                                     ? std::string::npos
                                                     : path_begin - host_begin);
  if (host_port.empty()) throw std::invalid_argument("缺少主机名: " + url);

  // 补全默认端口，这样日志和指标里的地址是统一的
  if (host_port.find(':') == std::string::npos) {
    host_port += scheme == "https" ? ":443" : ":80";
  }

  std::string path = path_begin == std::string::npos ? "" : url.substr(path_begin);
  while (!path.empty() && path.back() == '/') path.pop_back();

  return BaseUrl{scheme + "://" + host_port, path};
}

}  // namespace llmgw
