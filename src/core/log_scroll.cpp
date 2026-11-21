#include "core/log_scroll.h"

#include <algorithm>
#include <cstddef>
#include <limits>

namespace dnd {

LogScroll::LogScroll(int visible_lines)
    : visible_lines_(std::max(0, visible_lines)) {}

auto LogScroll::Scroll(int lines, std::size_t total_lines) -> void {
  offset_ = std::max(0, offset_ + lines);
  Clamp(total_lines);
  count_ = total_lines;
}

auto LogScroll::OnAppend(std::size_t total_lines) -> void {
  // Пользователь читает старые строки: увеличиваем отступ ровно на столько,
  // на сколько вырос лог.
  if (offset_ > 0 && total_lines > count_) {
    const std::size_t grown = total_lines - count_;
    const std::size_t shifted = static_cast<std::size_t>(offset_) + grown;
    offset_ =
        shifted > static_cast<std::size_t>(std::numeric_limits<int>::max())
            ? std::numeric_limits<int>::max()
            : static_cast<int>(shifted);
  }
  count_ = total_lines;
  Clamp(total_lines);
}

auto LogScroll::ToNewest() -> void { offset_ = 0; }

auto LogScroll::Clamp(std::size_t total_lines) -> void {
  const int total = static_cast<int>(std::min<std::size_t>(
      total_lines, static_cast<std::size_t>(std::numeric_limits<int>::max())));
  offset_ = std::clamp(offset_, 0, std::max(0, total - visible_lines_));
}

}  // namespace dnd
