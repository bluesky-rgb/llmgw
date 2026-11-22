#include "llmgw/stream/sse_parser.h"

#include <algorithm>
#include <utility>

namespace llmgw::stream {

namespace {

constexpr std::string_view kBom = "\xEF\xBB\xBF";

}  // namespace

std::size_t SseParser::find_line_end(std::size_t from, std::size_t* terminator_len) {
  for (std::size_t i = std::max(from, searched_); i < buffer_.size(); ++i) {
    const char c = buffer_[i];
    if (c == '\n') {
      *terminator_len = 1;
      return i;
    }
    if (c == '\r') {
      // 缓冲区的最后一个字节是 \r：不知道后面是不是 \n（\r\n 被切在两块里），等下一个字节再判断。
      // 这样每个事件的原始字节与切块方式无关（任务设计第 4 节）
      if (i + 1 == buffer_.size()) {
        searched_ = i;  // 这个 \r 下次还要再看
        return std::string::npos;
      }
      *terminator_len = buffer_[i + 1] == '\n' ? 2 : 1;
      return i;
    }
  }
  searched_ = buffer_.size();
  return std::string::npos;
}

void SseParser::process_line(std::string_view line) {
  if (line.starts_with(':')) return;  // 注释
  std::string_view field = line;
  std::string_view value;
  if (const auto colon = line.find(':'); colon != std::string_view::npos) {
    field = line.substr(0, colon);
    value = line.substr(colon + 1);
    if (value.starts_with(' ')) value.remove_prefix(1);  // 冒号后的一个空格不属于值
  }
  if (field == "data") {
    data_.append(value);
    data_ += '\n';
    has_data_ = true;
  } else if (field == "event") {
    event_ = std::string(value);
  }
  // id、retry 和未知字段：不需要解析，原始字节照常转发
}

void SseParser::feed(std::string_view bytes) {
  if (overflowed_) return;
  buffer_.append(bytes);
  if (!bom_checked_) {
    // 流开头的 BOM：解析时跳过，原始字节保留。不足 3 个字节且可能是 BOM 的前缀时先等
    const std::string_view head(buffer_);
    if (head.size() < kBom.size() && kBom.starts_with(head)) return;
    bom_checked_ = true;
    if (head.starts_with(kBom)) scan_ = kBom.size();
  }
  while (!overflowed_) {
    std::size_t term = 0;
    const std::size_t end = find_line_end(scan_, &term);
    if (end == std::string::npos) break;
    const std::string_view line(buffer_.data() + scan_, end - scan_);
    scan_ = end + term;
    if (line.empty()) {
      dispatch(scan_);  // 空行：事件结束
    } else {
      process_line(line);
    }
  }
  if (buffer_.size() - event_begin_ > max_event_bytes_) overflowed_ = true;
  compact();
}

void SseParser::dispatch(std::size_t end) {
  // 在交出时也检查大小：否则一个超大事件的结尾和空行在同一次喂入中到达时会被放行，
  // 而分成小块到达时会被拒绝，结果取决于切块方式（模糊测试发现）
  if (end - event_begin_ > max_event_bytes_) {
    overflowed_ = true;
    return;
  }
  SseEvent ev;
  ev.raw.assign(buffer_, event_begin_, end - event_begin_);
  if (has_data_) data_.pop_back();  // 去掉最后一个 "\n"
  ev.data = std::move(data_);
  ev.event = std::move(event_);
  ev.has_data = has_data_;
  data_.clear();
  event_.clear();
  has_data_ = false;
  ready_.push_back(std::move(ev));
  ++dispatched_;
  event_begin_ = end;
}

void SseParser::compact() {
  // 已经交出的前缀超过一半时才移动剩下的字节：均摊 O(1)，又不会让缓冲区无限增长
  if (event_begin_ == 0 || event_begin_ * 2 < buffer_.size()) return;
  buffer_.erase(0, event_begin_);
  scan_ -= event_begin_;
  searched_ = searched_ > event_begin_ ? searched_ - event_begin_ : 0;
  event_begin_ = 0;
}

std::optional<SseEvent> SseParser::next() {
  if (ready_.empty()) return std::nullopt;
  SseEvent ev = std::move(ready_.front());
  ready_.pop_front();
  return ev;
}

bool SseParser::finish() {
  if (overflowed_) return false;
  bom_checked_ = true;  // 整个响应体不足 3 个字节时，feed 一直在等
  // 最后一个字节是 \r：现在可以确定它是一个换行（不会再有 \n 了）
  if (scan_ < buffer_.size() && buffer_.back() == '\r') {
    const std::string_view line(buffer_.data() + scan_, buffer_.size() - 1 - scan_);
    scan_ = buffer_.size();
    if (line.empty()) {
      dispatch(scan_);
    } else {
      process_line(line);
    }
  }
  if (event_begin_ == buffer_.size()) return true;  // 没有剩下的字节
  if (scan_ < buffer_.size()) return false;         // 最后一行在中间断开：半个事件
  // 剩下的都是完整的行，只是少了表示事件结束的空行：补上与最后一行相同的换行
  const std::string_view rest(buffer_.data() + event_begin_, buffer_.size() - event_begin_);
  const std::string_view blank =
      rest.ends_with("\r\n") ? "\r\n" : (rest.ends_with('\r') ? "\r" : "\n");
  buffer_.append(blank);
  dispatch(buffer_.size());
  return !overflowed_;  // 补上空行后超过上限：最后一个事件没有交出，不能报告成功（独立审查 #2）
}

}  // namespace llmgw::stream
