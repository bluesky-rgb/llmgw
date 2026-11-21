#include <coroutine>
#include <string>

namespace llmgw::core {

struct Task {
  struct promise_type {
    Task get_return_object() { return {}; }
    std::suspend_never initial_suspend() noexcept { return {}; }
    std::suspend_never final_suspend() noexcept { return {}; }
    void return_void() {}
    void unhandled_exception() {}
  };
};

// ⑥ 协程参数使用引用
Task process(const std::string& name) {
  (void)name;
  co_return;
}

// ⑤ 命名不符合规范
int BadName(int a) { return a; }

// ④ 格式错误
int   badly_formatted ( int x ){return x+1;}

// ⑦ 没有 Issue 编号的 TODO
// TODO: 以后再改

}  // namespace llmgw::core
