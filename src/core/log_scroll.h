#pragma once

#include <cstddef>

namespace dnd {

// Прокрутка панели лога: 0 — самые новые строки, положительное смещение —
// сколько строк пользователь поднял вверх. Пока он читает старые строки,
// новые не сдвигают текст.
class LogScroll {
 public:
  explicit LogScroll(int visible_lines);

  // lines > 0 — вверх (к старым), lines < 0 — вниз.
  auto Scroll(int lines, std::size_t total_lines) -> void;
  // Пришла новая строка: удерживает читаемое место, если лог прокручен.
  auto OnAppend(std::size_t total_lines) -> void;
  // End — вернуться к новым строкам.
  auto ToNewest() -> void;
  // Меняет высоту страницы, например когда окно уменьшилось: отступ сразу
  // подрезается под новый размер.
  auto SetVisibleLines(int visible_lines) -> void;

  [[nodiscard]] auto Offset() const -> int { return offset_; }

 private:
  auto Clamp(std::size_t total_lines) -> void;

  int visible_lines_;
  int offset_ = 0;
  std::size_t count_ = 0;
};

}  // namespace dnd
