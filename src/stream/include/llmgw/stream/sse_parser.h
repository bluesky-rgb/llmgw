// 增量 SSE 解析器（WHATWG HTML 规范 9.2 “Server-sent events” 的解析部分）。纯计算，没有 I/O。
//
// 喂入任意切块的字节，取出完整的事件。切块方式不影响结果：这是任务 1.5 的验收标准，
// 测试中对每一个切分位置、随机切块和逐字节喂入都做了验证。
#pragma once

#include <cstddef>
#include <deque>
#include <optional>
#include <string>
#include <string_view>

namespace llmgw::stream {

/// 一个完整的事件。
struct SseEvent {
  std::string raw;  ///< 原始字节（包括结尾的空行），原样转发给客户端
  std::string data;  ///< 所有 data 字段用 "\n" 连接（规范的 data buffer，去掉了最后一个 "\n"）
  std::string event;  ///< event 字段；没有时为空（规范中默认是 "message"）
  bool has_data = false;  ///< 是否有 data 字段。只有注释（": ping"）的事件没有，不算首字
};

class SseParser {
 public:
  explicit SseParser(std::size_t max_event_bytes = std::size_t{4} << 20)
      : max_event_bytes_(max_event_bytes) {}

  /// 喂入一段字节。
  void feed(std::string_view bytes);

  /// 取出下一个完整的事件；没有时返回空。
  std::optional<SseEvent> next();

  /// 还没有构成完整事件的字节数超过了上限（上游一直不发空行）。之后不要再喂入
  [[nodiscard]] bool overflowed() const { return overflowed_; }

  /// 响应体结束时调用一次，之后用 next() 取出最后的事件（可能有零个、一个或两个）。
  /// 剩下的字节如果以换行结尾（只是少了表示事件结束的空行），补上空行作为最后一个事件；
  /// 剩下的字节在一行中间断开（半个事件，不能转发）或者已经超过上限时返回 false。
  bool finish();

  /// 已经缓冲、还没有构成完整事件的字节数
  [[nodiscard]] std::size_t buffered() const { return buffer_.size() - event_begin_; }

  /// 还没有构成完整事件的原始字节（finish() 返回 false 时，就是被放弃的那半个事件）
  [[nodiscard]] std::string_view pending_raw() const {
    return std::string_view(buffer_).substr(event_begin_);
  }
  /// 最后一行中没有换行结束的部分
  [[nodiscard]] std::string_view unterminated_line() const {
    return std::string_view(buffer_).substr(scan_);
  }
  /// 交出过的事件数（包括还在队列中、没有被取出的）
  [[nodiscard]] std::size_t dispatched() const { return dispatched_; }

 private:
  // 从 scan_ 开始找下一个换行，返回换行的起点；没有完整的换行时返回 npos
  std::size_t find_line_end(std::size_t from, std::size_t* terminator_len);
  void process_line(std::string_view line);
  void dispatch(std::size_t end);  // 当前事件到 end 为止（原始字节 [event_begin_, end)）
  void compact();

  std::size_t max_event_bytes_;
  std::string buffer_;           ///< [event_begin_, size) 是还没有构成完整事件的字节
  std::size_t event_begin_ = 0;  ///< 当前事件的原始字节从这里开始
  std::size_t scan_ = 0;         ///< 已经按行处理到这里
  /// [scan_, searched_) 中确定没有换行：一行很长、分很多次到达时，不从行首重新搜索（否则是
  /// O(n²)，S9）
  std::size_t searched_ = 0;
  std::size_t dispatched_ = 0;
  bool bom_checked_ = false;
  bool overflowed_ = false;
  // 当前事件
  std::string data_;
  std::string event_;
  bool has_data_ = false;
  std::deque<SseEvent> ready_;  ///< 已经完整、等待取出的事件
};

}  // namespace llmgw::stream
