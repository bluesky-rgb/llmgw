// Result<T>：可能失败的操作的返回类型。
//
// 实现使用 tl::expected（接口与 C++23 std::expected 一致）。Clang 18 + libstdc++ 13
// 下 std::expected 不可用，见 ADR-007。代码中只使用 Result 和 fail()，不直接写 tl::，
// 以后工具链支持 std::expected 时只需修改这里的别名。
#pragma once

#include <string>
#include <tl/expected.hpp>
#include <utility>

#include "llmgw/core/error.h"

namespace llmgw {

/// 成功时持有 T，失败时持有 Error。用法：
/// ```
/// Result<Target> pick() {
///   if (targets.empty()) return fail(ErrorCode::kModelNotFound, "no target");
///   return targets.front();
/// }
/// if (auto r = pick(); r) use(*r); else log(r.error().message);
/// ```
/// 支持 `and_then`、`transform`、`or_else` 等组合操作（与 std::expected 相同）。
template <class T>
using Result = tl::expected<T, Error>;

/// 失败结果的类型，用于 `return fail(...)`。
using Failure = tl::unexpected<Error>;

/// 构造一个失败结果。
inline Failure fail(Error error) { return Failure(std::move(error)); }

/// 构造一个失败结果（不带 Retry-After）。
inline Failure fail(ErrorCode code, std::string message) {
  return Failure(Error{.code = code, .message = std::move(message), .retry_after = {}});
}

}  // namespace llmgw
