#include "http_util.h"

#include <simdjson.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <nlohmann/json.hpp>

namespace llmgw::server::detail {
namespace {

using Json = nlohmann::json;

bool iequals(std::string_view a, std::string_view b) {
  return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
           return std::tolower(static_cast<unsigned char>(x)) ==
                  std::tolower(static_cast<unsigned char>(y));
         });
}

Json error_json(const Error& error) {
  return Json{{"error",
               {{"message", error.message},
                {"type", std::string(error_type(error.code))},
                {"code", std::string(code_name(error.code))}}}};
}

}  // namespace

std::string error_body(const Error& error) { return error_json(error).dump(); }

std::string error_event(const Error& error) { return "data: " + error_json(error).dump() + "\n\n"; }

std::string request_id(std::string_view from_client, std::mt19937_64& rng) {
  const bool valid =
      !from_client.empty() && from_client.size() <= 64 &&
      std::all_of(from_client.begin(), from_client.end(), [](char c) {
        return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '.' || c == '_' || c == '-';
      });
  if (valid) return std::string(from_client);
  static constexpr std::array<char, 16> kHex{'0', '1', '2', '3', '4', '5', '6', '7',
                                             '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};
  std::uint64_t v = rng();
  std::string id(16, '0');
  for (auto& c : id) {
    c = kHex.at(v & 0xF);
    v >>= 4;
  }
  return id;
}

namespace {

// parse_chat_fields 的两个字段：出错时返回给客户端的说明，成功时返回空串
using JsonValue = simdjson::simdjson_result<simdjson::ondemand::value>;

std::string read_model(JsonValue value, ChatFields* f, bool* seen) {
  // 重复的 model：网关按哪一个路由、上游又按哪一个处理，不同的解析器结论不同（独立审查发现）
  if (*seen) return "model 出现了不止一次";
  *seen = true;
  std::string_view model;
  if (value.get_string().get(model) != simdjson::SUCCESS || model.empty()) {
    return "model 必须是非空字符串";
  }
  f->model = std::string(model);
  return {};
}

std::string read_stream(JsonValue value, ChatFields* f, bool* seen) {
  if (*seen) return "stream 出现了不止一次";
  *seen = true;
  simdjson::ondemand::value v;
  if (value.get(v) != simdjson::SUCCESS) return "请求体不是合法的 JSON";
  bool is_null = false;
  if (v.is_null().get(is_null) == simdjson::SUCCESS && is_null) return {};  // null 等同于不传
  if (v.get_bool().get(f->stream) != simdjson::SUCCESS) return "stream 必须是 true 或 false";
  return {};
}

}  // namespace

Result<ChatFields> parse_chat_fields(std::string* body) {
  namespace od = simdjson::ondemand;
  thread_local od::parser parser;  // 每个 Worker 线程一个，复用内部缓冲区
  body->reserve(body->size() + simdjson::SIMDJSON_PADDING);
  const simdjson::padded_string_view view(body->data(), body->size(), body->capacity());

  auto invalid = [](std::string message) {
    return fail(ErrorCode::kInvalidRequest, std::move(message));
  };
  od::document doc;
  if (parser.iterate(view).get(doc) != simdjson::SUCCESS) return invalid("请求体不是合法的 JSON");
  od::object obj;
  if (auto err = doc.get_object().get(obj); err != simdjson::SUCCESS) {
    return invalid(err == simdjson::INCORRECT_TYPE ? "请求体必须是一个 JSON 对象"
                                                   : "请求体不是合法的 JSON");
  }
  ChatFields f;
  bool have_model = false;
  bool have_stream = false;
  for (auto field : obj) {
    std::string_view key;
    if (field.unescaped_key().get(key) != simdjson::SUCCESS)
      return invalid("请求体不是合法的 JSON");
    std::string problem;
    if (key == "model") {
      problem = read_model(field.value(), &f, &have_model);
    } else if (key == "stream") {
      problem = read_stream(field.value(), &f, &have_stream);
    }
    if (!problem.empty()) return invalid(std::move(problem));
  }
  if (!have_model) return invalid("缺少必填字段：model");
  if (!doc.at_end()) return invalid("请求体不是合法的 JSON（对象之后还有多余的内容）");
  return f;
}

std::string models_body(const config::Snapshot& snapshot) {
  Json data = Json::array();
  for (const auto& m : snapshot.models) {
    data.push_back({{"id", m.name}, {"object", "model"}, {"created", 0}, {"owned_by", "llmgw"}});
  }
  return Json{{"object", "list"}, {"data", std::move(data)}}.dump();
}

std::string_view path_of(std::string_view target) {
  for (const std::string_view scheme :
       {std::string_view("http://"), std::string_view("https://")}) {
    if (target.size() >= scheme.size() && iequals(target.substr(0, scheme.size()), scheme)) {
      const auto slash = target.find('/', scheme.size());
      target = slash == std::string_view::npos ? std::string_view("/") : target.substr(slash);
      break;
    }
  }
  return target.substr(0, target.find('?'));
}

bool safe_header(std::string_view name, std::string_view value) {
  // RFC 9110 5.6.2 tchar
  constexpr std::string_view kTokenExtra = "!#$%&'*+-.^_`|~";
  const bool name_ok = !name.empty() && std::all_of(name.begin(), name.end(), [&](char c) {
    return std::isalnum(static_cast<unsigned char>(c)) != 0 ||
           kTokenExtra.find(c) != std::string_view::npos;
  });
  const bool value_ok = std::none_of(value.begin(), value.end(), [](char c) {
    const auto u = static_cast<unsigned char>(c);
    return (u < 0x20 && u != '\t') || u == 0x7F;
  });
  return name_ok && value_ok;
}

bool is_plain_chunked(std::string_view te) {
  while (!te.empty() && (te.front() == ' ' || te.front() == '\t')) te.remove_prefix(1);
  while (!te.empty() && (te.back() == ' ' || te.back() == '\t')) te.remove_suffix(1);
  return iequals(te, "chunked");
}

bool is_hop_by_hop(std::string_view name) {
  static constexpr std::array<std::string_view, 7> kHopByHop{
      "connection", "keep-alive", "transfer-encoding", "content-length", "te",
      "trailer",    "upgrade"};
  return std::any_of(kHopByHop.begin(), kHopByHop.end(),
                     [&](std::string_view h) { return iequals(h, name); });
}

std::string bearer_token(std::string_view authorization) {
  constexpr std::string_view kScheme = "bearer";
  if (authorization.size() <= kScheme.size() ||
      !iequals(authorization.substr(0, kScheme.size()), kScheme)) {
    return {};
  }
  std::string_view rest = authorization.substr(kScheme.size());
  if (rest.front() != ' ') return {};  // "Bearerxyz" 不是 Bearer
  while (!rest.empty() && rest.front() == ' ') rest.remove_prefix(1);
  while (!rest.empty() && (rest.back() == ' ' || rest.back() == '\t')) rest.remove_suffix(1);
  if (rest.empty() || rest.find_first_of(" \t") != std::string_view::npos) return {};
  return std::string(rest);
}

bool plugin_header_allowed(std::string_view name, std::string_view value) {
  static constexpr std::array<std::string_view, 6> kGatewayOwned{
      "content-type", "x-request-id", "x-should-retry", "retry-after", "retry-after-ms", "allow"};
  return safe_header(name, value) && !is_hop_by_hop(name) &&
         std::none_of(kGatewayOwned.begin(), kGatewayOwned.end(),
                      [&](std::string_view h) { return iequals(h, name); });
}

}  // namespace llmgw::server::detail
