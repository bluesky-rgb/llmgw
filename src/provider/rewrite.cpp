#include "rewrite.h"

#include <simdjson.h>

#include <functional>
#include <map>
#include <memory>
#include <set>

namespace llmgw::provider::detail {

namespace od = simdjson::ondemand;

namespace {

// ---------- 规则树：按层组织的改写规则 ----------

struct Node;

/// 作用于一个对象中名为 key 的字段的操作
struct FieldOps {
  bool drop = false;
  std::optional<std::string> rename_to;
  std::optional<std::string> set_json;  ///< 替换（或创建）为这个 JSON 文本
  std::unique_ptr<Node> into_object;    ///< 值是对象时向下
  std::unique_ptr<Node> into_each;      ///< 值是数组时，对每个对象元素向下
};

struct Node {
  std::map<std::string, FieldOps, std::less<>> fields;
  bool creates = false;  ///< 子树中有设置：路径不存在时要创建
  [[nodiscard]] const FieldOps* find(std::string_view key) const {
    auto it = fields.find(key);
    return it == fields.end() ? nullptr : &it->second;
  }
};

// 沿路径走到最后一段所在的节点，中间节点按需创建。creating：这条路径是设置，沿途标记 creates
// （带 [] 的段不会创建数组：没有元素可以设置）
Node* walk(Node* root, const config::FieldPath& path, bool creating) {
  Node* n = root;
  bool can_create = creating;
  for (std::size_t i = 0; i + 1 < path.size(); ++i) {
    if (can_create) n->creates = true;
    auto& f = n->fields[path[i].name];
    auto& child = path[i].each ? f.into_each : f.into_object;
    if (!child) child = std::make_unique<Node>();
    if (path[i].each) can_create = false;
    n = child.get();
  }
  if (creating) n->creates = true;  // 数组的每个元素中缺少这个字段时，在元素中创建
  return n;
}

bool is_prefix(const config::FieldPath& prefix, const std::vector<std::string>& path) {
  if (prefix.size() > path.size()) return false;
  for (std::size_t i = 0; i < prefix.size(); ++i) {
    if (prefix[i].name != path[i]) return false;
  }
  return true;
}

// 两条路径在同一条线上（一条是另一条的前缀，按字段名比较）
bool overlaps(const config::FieldPath& a, const std::vector<std::string>& b) {
  if (is_prefix(a, b)) return true;
  if (a.size() <= b.size()) return false;
  for (std::size_t i = 0; i < b.size(); ++i) {
    if (a[i].name != b[i]) return false;
  }
  return true;
}

// 内置改写是否与配置的规则冲突（配置的路径与它在同一条线上）：冲突时让给配置
bool overridden_by_config(const Rules& rules, const std::vector<std::string>& builtin) {
  for (const auto& p : rules.drops) {
    if (overlaps(p, builtin)) return true;
  }
  for (const auto& r : rules.renames) {
    if (overlaps(r.from, builtin) || overlaps(r.to, builtin)) return true;
  }
  for (const auto& s : rules.sets) {
    if (overlaps(s.path, builtin)) return true;
  }
  return false;
}

// 请求体中是否有 stream_options.include_usage == true。键按解码后的名字匹配（与改写器、server
// 一致）； 重复的键：任意一项为 true 即为 true（独立审查 1.6 #1）
bool asks_for_usage(const std::string& buf) {
  od::parser parser;
  od::document doc;
  const simdjson::padded_string_view view(buf.data(), buf.size(), buf.capacity());
  od::object root;
  if (parser.iterate(view).get(doc) != simdjson::SUCCESS ||
      doc.get_object().get(root) != simdjson::SUCCESS) {
    return false;
  }
  for (auto field : root) {
    std::string_view key;
    if (field.unescaped_key().get(key) != simdjson::SUCCESS) return false;
    if (key != "stream_options") continue;
    od::object so;
    if (field.value().get_object().get(so) != simdjson::SUCCESS) continue;
    for (auto inner : so) {
      std::string_view k;
      if (inner.unescaped_key().get(k) != simdjson::SUCCESS) return false;
      if (k != "include_usage") continue;
      bool b = false;
      if (inner.value().get_bool().get(b) == simdjson::SUCCESS && b) return true;
    }
  }
  return false;
}

config::FieldPath plain_path(std::initializer_list<std::string_view> names) {
  config::FieldPath p;
  for (auto n : names) p.push_back({.name = std::string(n), .each = false});
  return p;
}

Node build(const Rules& rules, bool include_usage) {
  Node root;
  auto set = [&](const config::FieldPath& path, std::string json) {
    walk(&root, path, true)->fields[path.back().name].set_json = std::move(json);
  };
  if (!overridden_by_config(rules, {"model"})) set(plain_path({"model"}), json_string(rules.model));
  if (include_usage && !overridden_by_config(rules, {"stream_options", "include_usage"})) {
    set(plain_path({"stream_options", "include_usage"}), "true");
  }
  for (const auto& p : rules.drops) walk(&root, p, false)->fields[p.back().name].drop = true;
  for (const auto& r : rules.renames)
    walk(&root, r.from, false)->fields[r.from.back().name].rename_to = r.to.back().name;
  for (const auto& s : rules.sets) set(s.path, s.json_value);
  return root;
}

// ---------- 按字节位置改写 ----------

std::string_view trim(std::string_view s) {
  auto ws = [](char c) { return c == ' ' || c == '\n' || c == '\r' || c == '\t'; };
  while (!s.empty() && ws(s.back())) s.remove_suffix(1);
  while (!s.empty() && ws(s.front())) s.remove_prefix(1);
  return s;
}

struct Item {
  std::string key;           ///< 解码后的键名（用于匹配）
  std::string_view raw_key;  ///< 原文中的键（不含引号；没有改名时原样输出）
  std::string_view value;    ///< 原文中的值
};

// 逐项写出 {"k":v,...}
struct ObjectWriter {
  std::string* out;
  bool first = true;
  void key(std::string_view json_key) {
    if (!first) out->push_back(',');
    first = false;
    out->append(json_key);
    out->push_back(':');
  }
};

// 改名的目标名已经存在：原来的那一项被删除（改名的结果覆盖它）
std::set<std::string, std::less<>> rename_targets(const std::vector<Item>& items,
                                                  const Node& node) {
  std::set<std::string, std::less<>> targets;
  for (const auto& it : items) {
    const FieldOps* ops = node.find(it.key);
    if (ops != nullptr && !ops->drop && ops->rename_to) targets.insert(*ops->rename_to);
  }
  return targets;
}

class Rewriter {
 public:
  /// [begin, end) 是原文，后面至少还有 SIMDJSON_PADDING 个可读的字节
  Rewriter(const char* begin, const char* end) : begin_(begin), end_(end) {}

  bool object(std::string_view raw, const Node& node, std::string* out);
  bool array(std::string_view raw, const Node& node, std::string* out);
  bool value(std::string_view raw, const FieldOps* ops, std::string* out);

 private:
  bool collect(std::string_view raw, std::string* holder, std::vector<Item>* items);
  bool create_missing(const Node& node, const std::set<std::string, std::less<>>& present,
                      ObjectWriter* w);

  // simdjson 要求输入后面有 SIMDJSON_PADDING 个可读的字节。原文中的片段后面是原文的其余部分和填充，
  // 直接使用；不在原文中的（设置的值、创建的 "{}"）复制到 holder（调用者保证 holder
  // 在使用期间有效）
  simdjson::padded_string_view view(std::string_view raw, std::string* holder) const {
    // std::less：比较不同分配的指针也有定义（独立审查 1.6 #10）
    const std::less<> before;
    if (!before(raw.data(), begin_) && !before(end_, raw.data() + raw.size())) {
      return simdjson::padded_string_view(
          raw.data(), raw.size(),
          static_cast<std::size_t>(end_ - raw.data()) + simdjson::SIMDJSON_PADDING);
    }
    holder->reserve(raw.size() + simdjson::SIMDJSON_PADDING);
    holder->assign(raw);
    return simdjson::padded_string_view(holder->data(), holder->size(), holder->capacity());
  }

  od::parser parser_;
  const char* begin_;
  const char* end_;
};

// 收集对象的每一项在原文中的位置。不是对象、对象之后还有内容时返回 false
bool Rewriter::collect(std::string_view raw, std::string* holder, std::vector<Item>* items) {
  od::document doc;
  if (parser_.iterate(view(raw, holder)).get(doc) != simdjson::SUCCESS) return false;
  od::object obj;
  if (doc.get_object().get(obj) != simdjson::SUCCESS) return false;
  for (auto field : obj) {
    std::string_view raw_key;
    if (field.escaped_key().get(raw_key) != simdjson::SUCCESS) return false;
    std::string_view key;
    if (field.unescaped_key().get(key) != simdjson::SUCCESS) return false;
    std::string k(key);  // unescaped_key 指向解析器内部的缓冲区，下一次解析时失效
    od::value v;
    if (field.value().get(v) != simdjson::SUCCESS) return false;
    std::string_view rv;
    if (v.raw_json().get(rv) != simdjson::SUCCESS) return false;
    items->push_back({std::move(k), raw_key, trim(rv)});
  }
  return doc.at_end();  // 对象之后还有内容：不合法
}

// 递归的深度由规则树决定（配置的路径最多 32 段），与客户端请求体的嵌套深度无关
// NOLINTNEXTLINE(misc-no-recursion)
bool Rewriter::object(std::string_view raw, const Node& node, std::string* out) {
  std::string holder;  // 整个函数期间有效：items 中的位置可能指向它
  std::vector<Item> items;
  if (!collect(raw, &holder, &items)) return false;
  const auto targets = rename_targets(items, node);
  std::set<std::string, std::less<>> present;  // 只在这一层需要创建字段时记录（避免每条消息都分配）
  out->push_back('{');
  ObjectWriter w{out};
  for (const auto& it : items) {
    const FieldOps* ops = node.find(it.key);
    if (ops != nullptr && ops->drop) continue;
    const bool renamed = ops != nullptr && ops->rename_to.has_value();
    if (!renamed && targets.contains(it.key)) continue;
    const std::string& name = renamed ? *ops->rename_to : it.key;
    w.key(renamed ? json_string(name) : std::string("\"").append(it.raw_key).append("\""));
    if (node.creates) present.insert(name);
    if (!value(it.value, node.find(name), out)) return false;
  }
  if (node.creates && !create_missing(node, present, &w)) return false;
  out->push_back('}');
  return true;
}

// 创建不存在的字段：设置的值，或者通向设置的中间对象
// NOLINTNEXTLINE(misc-no-recursion)
bool Rewriter::create_missing(const Node& node, const std::set<std::string, std::less<>>& present,
                              ObjectWriter* w) {
  for (const auto& [name, ops] : node.fields) {
    if (present.contains(name)) continue;
    if (!ops.set_json && !(ops.into_object && ops.into_object->creates)) continue;
    w->key(json_string(name));
    const std::string_view v =
        ops.set_json ? std::string_view(*ops.set_json) : std::string_view("{}");
    if (!value(v, &ops, w->out)) return false;
  }
  return true;
}

// NOLINTNEXTLINE(misc-no-recursion)
bool Rewriter::array(std::string_view raw, const Node& node, std::string* out) {
  std::string holder;
  std::vector<std::pair<bool, std::string_view>> elements;  // 是否对象、原文
  {
    od::document doc;
    if (parser_.iterate(view(raw, &holder)).get(doc) != simdjson::SUCCESS) return false;
    od::array arr;
    if (doc.get_array().get(arr) != simdjson::SUCCESS) return false;
    for (auto el : arr) {
      od::value v;
      if (el.get(v) != simdjson::SUCCESS) return false;
      od::json_type type{};
      if (v.type().get(type) != simdjson::SUCCESS) return false;
      std::string_view rv;
      if (v.raw_json().get(rv) != simdjson::SUCCESS) return false;
      elements.emplace_back(type == od::json_type::object, trim(rv));
    }
    if (!doc.at_end()) return false;
  }
  out->push_back('[');
  for (std::size_t i = 0; i < elements.size(); ++i) {
    if (i > 0) out->push_back(',');
    const auto& [is_object, text] = elements[i];
    if (is_object) {
      if (!object(text, node, out)) return false;
    } else {
      out->append(text);
    }
  }
  out->push_back(']');
  return true;
}

// NOLINTNEXTLINE(misc-no-recursion)
bool Rewriter::value(std::string_view raw, const FieldOps* ops, std::string* out) {
  if (ops == nullptr) {
    out->append(raw);  // 没有被改写路径经过：逐字节保留
    return true;
  }
  const std::string_view v = ops->set_json ? std::string_view(*ops->set_json) : raw;
  const char first = v.empty() ? '\0' : v.front();
  if (ops->into_object && first == '{') return object(v, *ops->into_object, out);
  if (ops->into_object && ops->into_object->creates)
    return object("{}", *ops->into_object, out);  // 不是对象：替换
  if (ops->into_each && first == '[') return array(v, *ops->into_each, out);
  out->append(v);
  return true;
}

}  // namespace

std::string json_string(std::string_view s) {
  std::string out = "\"";
  for (const char c : s) {
    const auto u = static_cast<unsigned char>(c);
    if (c == '"' || c == '\\') {
      out += '\\';
      out += c;
    } else if (u < 0x20) {
      constexpr std::string_view kHex = "0123456789abcdef";
      out += "\\u00";
      out += kHex[u >> 4U];
      out += kHex[u & 0xFU];
    } else {
      out += c;
    }
  }
  return out + "\"";
}

Result<Rewritten> rewrite(std::string_view body, const Rules& rules) {
  // 复制到一块带填充的缓冲区（1 MB 约 0.05 ms）：之后的所有片段都在其中，不需要再复制
  std::string buf;
  buf.reserve(body.size() + simdjson::SIMDJSON_PADDING);
  buf.assign(body);
  const std::string_view text = trim(buf);
  if (text.empty() || text.front() != '{')
    return fail(ErrorCode::kInvalidRequest, "请求体必须是 JSON 对象");

  // 客户端自己是否要求了 include_usage（要求了，usage 块就不能去掉）
  const bool client_usage = rules.include_usage && asks_for_usage(buf);
  const Node tree = build(rules, rules.include_usage);
  Rewriter rw(buf.data(), buf.data() + buf.size());
  Rewritten r;
  r.body.reserve(body.size() + 128);
  if (!rw.object(text, tree, &r.body))
    return fail(ErrorCode::kInvalidRequest, "请求体不是合法的 JSON 对象");
  // 发给上游的请求要求了 usage，而客户端没有要求：stream 去掉 usage 块。内置改写生效时一定要求了；
  // 让给了配置时（独立审查 1.6 #5：比如配置自己设置了 include_usage = true）检查改写的结果
  if (rules.include_usage && !client_usage) {
    if (!overridden_by_config(rules, {"stream_options", "include_usage"})) {
      r.injected_include_usage = true;
    } else {
      std::string copy;
      copy.reserve(r.body.size() + simdjson::SIMDJSON_PADDING);
      copy.assign(r.body);
      r.injected_include_usage = asks_for_usage(copy);
    }
  }
  return r;
}

}  // namespace llmgw::provider::detail
