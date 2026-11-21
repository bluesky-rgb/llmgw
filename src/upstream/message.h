// 请求报文的构造、最终响应头的读取。
#pragma once

#include <boost/beast/http.hpp>
#include <memory>

#include "connection.h"
#include "establish.h"
#include "llmgw/upstream/client.h"
#include "pool.h"

namespace llmgw::upstream::detail {

using Message = boost::beast::http::request<boost::beast::http::string_body>;
/// 响应体直接读进调用者的缓冲区（Body::read 的 out），不另外分配
using Parser = boost::beast::http::response_parser<boost::beast::http::buffer_body>;

/// 构造请求报文。调用者的请求头中：Client 自己负责的（Host、Content-Length 等）被忽略；
/// 名字或值不合法的（含 CR/LF 等，会造成请求头注入）被丢弃并记录日志。
/// 方法必须是 token；目标必须以 "/" 开头且不含空白和控制字符（否则可以注入请求行，独立审查 R4）。
/// 不合法时返回 kSend 失败（maybe_sent = false）
Outcome<Message> build_message(const Route& route, Request request);

/// 字段名是否是 RFC 9110 的 token、值中是否没有 HTAB 以外的控制字符
bool valid_header(std::string_view name, std::string_view value);

/// 响应体读取器（Body 的实现）。parser 已经读完响应头
std::unique_ptr<Body> make_body(Lease lease, std::unique_ptr<Parser> parser);

/// 读响应头，跳过 1xx（101 按错误处理）。失败的 maybe_sent 都是 true；
/// reused_without_response 表示一个字节都没有收到（由调用者再结合“是否复用的连接”判断）
/// head_request：HEAD 请求的响应没有响应体（不论 Content-Length 写多少，独立审查 R5）
Task<Outcome<std::unique_ptr<Parser>>> read_final_head(Connection* conn, bool head_request);

}  // namespace llmgw::upstream::detail
