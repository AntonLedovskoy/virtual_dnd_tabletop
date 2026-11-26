#pragma once

namespace dnd {

// Клетка сетки. Отдельный заголовок, чтобы о клетке могли говорить модели,
// которым не нужен весь GridMap (например, поле света в core/light.h).
struct Cell {
  int x = 0;
  int y = 0;

  friend auto operator==(const Cell&, const Cell&) -> bool = default;
};

}  // namespace dnd
