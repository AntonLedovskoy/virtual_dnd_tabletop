#pragma once

#include <vector>

#include "core/grid_map.h"

namespace dnd {

// Клетки линии от from до to включительно (алгоритм Брезенхема): быстрый
// взмах мышью не оставляет дыр между далёкими клетками.
[[nodiscard]] auto LineCells(Cell from, Cell to) -> std::vector<Cell>;

// Штрих кисти редактора: помнит последнюю клетку и отдаёт участок линии до
// новой. Выход за карту фильтрует вызывающий код.
class PaintDrag {
 public:
  auto Begin(Cell cell) -> void;
  // Пусто, если клетка не сменилась; иначе линия от прошлой клетки до новой.
  auto Move(Cell cell) -> std::vector<Cell>;
  auto End() -> void;
  [[nodiscard]] auto Active() const -> bool { return active_; }

 private:
  bool active_ = false;
  Cell last_{};
};

}  // namespace dnd
