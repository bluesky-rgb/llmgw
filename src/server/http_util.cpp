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
  for (auto field : obj) {
    std::string_view key;
    if (field.unescaped_key().get(key) != simdjson::SUCCESS)
      return invalid("请求体不是合法的 JSON");
    if (key == "model") {
      std::string_view model;
      if (field.value().get_string().get(model) != simdjson::SUCCESS || model.empty()) {
        return invalid("model 必须是非空字符串");
      }
      f.model = std::string(model);
      have_model = true;
    } else if (key == "stream") {
      od::value v;
      if (field.value().get(v) != simdjson::SUCCESS) return invalid("请求体不是合法的 JSON");
      bool is_null = false;
      if (v.is_null().get(is_null) == simdjson::SUCCESS && is_null) continue;
      if (v.get_bool().get(f.stream) != simdjson::SUCCESS)
        return invalid("stream 必须是 true 或 false");
    }
  }
  if (!have_model) return invalid("缺少必填字段：model");
  return f;
}

std::string models_body(const config::Snapshot& snapshot) {
  Json data = Json::array();
  for (const auto& m : snapshot.models) {
    data.push_back({{"id", m.name}, {"object", "model"}, {"created", 0}, {"owned_by", "llmgw"}});
  }
  return Json{{"object", "list"}, {"data", std::move(data)}}.dump();
}

std::string_view path_of(std::string_view target) { return target.substr(0, target.find('?')); }

bool is_hop_by_hop(std::string_view name) {
  static constexpr std::array<std::string_view, 7> kHopByHop{
      "connection", "keep-alive", "transfer-encoding", "content-length", "te",
      "trailer",    "upgrade"};
  return std::any_of(kHopByHop.begin(), kHopByHop.end(),
                     [&](std::string_view h) { return iequals(h, name); });
}

}  // namespace llmgw::server::detail
