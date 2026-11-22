// 模糊测试：请求体改写（任务 1.6 P8）。输入是客户端的请求体，改写规则固定（覆盖三种改写、[]、
// 嵌套创建、改名覆盖已有的项）。不变式：prepare() 成功时，
// - 输入是合法的 JSON（simdjson DOM 能解析）→ 输出也是合法的 JSON 对象
// - 输出的每一个 model 都等于目标的模型名；被删除、被改名的字段不存在；被设置的字段等于配置的值；
//   改名的结果覆盖原来的同名项
// - 网关加上了 include_usage ⇔ 输入中没有 stream_options.include_usage ==
// true（键解码后比较，重复的键任意一项）
#include <simdjson.h>

#include <cstdint>
#include <string>
#include <string_view>

#include "llmgw/config/loader.h"
#include "llmgw/provider/provider.h"

namespace {

using namespace llmgw;  // NOLINT(google-build-using-namespace)
namespace dom = simdjson::dom;

const config::TargetConfig& target() {
  static const config::TargetConfig t = [] {
    config::TargetConfig c;
    c.provider = "p";
    c.model = "real-model";
    auto path = [](std::string_view s) { return *config::parse_field_path(s); };
    c.drop_fields = {path("thinking"), path("messages[].reasoning_content")};
    c.rename_fields = {{.from = path("max_tokens"), .to = path("max_completion_tokens")},
                       {.from = path("messages[].a"), .to = path("messages[].b")}};
    c.set_fields = {{.path = path("reasoning_effort"), .json_value = R"("low")"},
                    {.path = path("extra.deep.type"), .json_value = R"("disabled")"},
                    {.path = path("messages[].name"), .json_value = R"("n")"}};
    return c;
  }();
  return t;
}

bool is_string(dom::element e, std::string_view want) {
  std::string_view s;
  return e.get_string().get(s) == simdjson::SUCCESS && s == want;
}

// 对象中名为 key 的每一项（可能重复）都满足 pred；absent_ok：没有这一项也可以
template <class Pred>
bool all_fields(dom::object o, std::string_view key, Pred pred, bool absent_ok) {
  bool found = false;
  for (auto field : o) {
    if (field.key != key) continue;
    found = true;
    if (!pred(field.value)) return false;
  }
  return found || absent_ok;
}

bool absent(dom::object o, std::string_view key) {
  return all_fields(o, key, [](dom::element) { return false; }, true);
}

void check(std::string_view input, const provider::Prepared& out) {
  dom::parser in_parser;
  dom::element in_root;
  if (in_parser.parse(input.data(), input.size()).get(in_root) != simdjson::SUCCESS)
    return;  // 输入本身不合法
  dom::object in_obj;
  if (in_root.get_object().get(in_obj) != simdjson::SUCCESS)
    __builtin_trap();  // prepare 只接受对象

  dom::parser parser;
  dom::element root;
  if (parser.parse(out.request.body).get(root) != simdjson::SUCCESS) __builtin_trap();
  dom::object o;
  if (root.get_object().get(o) != simdjson::SUCCESS) __builtin_trap();

  if (!all_fields(o, "model", [](dom::element e) { return is_string(e, "real-model"); }, false))
    __builtin_trap();
  if (!absent(o, "thinking") || !absent(o, "max_tokens")) __builtin_trap();
  // 改名：输入中只有一个 max_tokens 时，输出的每一个 max_completion_tokens
  // 都是它的值（原来的同名项被覆盖）
  std::size_t renamed = 0;
  std::string renamed_value;
  for (auto field : in_obj) {
    if (field.key != "max_tokens") continue;
    ++renamed;
    renamed_value = simdjson::minify(field.value);
  }
  if (renamed == 1 &&
      !all_fields(
          o, "max_completion_tokens",
          [&](dom::element e) { return simdjson::minify(e) == renamed_value; }, false)) {
    __builtin_trap();
  }
  if (!all_fields(o, "reasoning_effort", [](dom::element e) { return is_string(e, "low"); }, false))
    __builtin_trap();
  auto deep_type = [](dom::element e) {
    dom::object x;
    if (e.get_object().get(x) != simdjson::SUCCESS) return false;
    return all_fields(
        x, "deep",
        [](dom::element d) {
          dom::object y;
          if (d.get_object().get(y) != simdjson::SUCCESS) return false;
          return all_fields(
              y, "type", [](dom::element t) { return is_string(t, "disabled"); }, false);
        },
        false);
  };
  if (!all_fields(o, "extra", deep_type, false)) __builtin_trap();
  auto messages = [](dom::element e) {
    dom::array arr;
    if (e.get_array().get(arr) != simdjson::SUCCESS) return true;  // 不是数组：原样保留
    for (dom::element el : arr) {
      dom::object m;
      if (el.get_object().get(m) != simdjson::SUCCESS) continue;  // 不是对象的元素：原样保留
      if (!absent(m, "reasoning_content") || !absent(m, "a")) return false;
      if (!all_fields(m, "name", [](dom::element n) { return is_string(n, "n"); }, false))
        return false;
    }
    return true;
  };
  if (!all_fields(o, "messages", messages, true)) __builtin_trap();

  // include_usage：输出中一定为 true；客户端自己要求了时不能去掉 usage 块
  auto usage_true = [](dom::element e) {
    dom::object so;
    if (e.get_object().get(so) != simdjson::SUCCESS) return false;
    return all_fields(
        so, "include_usage",
        [](dom::element u) {
          bool b = false;
          return u.get_bool().get(b) == simdjson::SUCCESS && b;
        },
        false);
  };
  if (!all_fields(o, "stream_options", usage_true, false)) __builtin_trap();
  // 客户端是否要求了：DOM 的键已经解码；重复的键中任意一项为 true 即为要求了（与改写器的约定一致）
  bool client_asked = false;
  for (auto field : in_obj) {
    dom::object so;
    if (field.key != "stream_options" || field.value.get_object().get(so) != simdjson::SUCCESS)
      continue;
    for (auto inner : so) {
      bool b = false;
      if (inner.key == "include_usage" && inner.value.get_bool().get(b) == simdjson::SUCCESS && b) {
        client_asked = true;
      }
    }
  }
  if (out.strip_usage_chunk == client_asked) __builtin_trap();
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  config::Provider p;
  p.name = "p";
  p.base_url = {.scheme = "https", .host = "h", .port = 443, .base_path = "/v1"};
  p.api_key = "sk-FUZZ";
  RequestInfo req;
  req.model = "smart";
  req.stream = true;
  req.body.assign(reinterpret_cast<const char*>(data), size);
  auto out = provider::prepare(req, p, target());
  if (!out) {
    if (out.error().message.find("sk-FUZZ") != std::string::npos) __builtin_trap();
    return 0;
  }
  check(req.body, *out);
  return 0;
}
