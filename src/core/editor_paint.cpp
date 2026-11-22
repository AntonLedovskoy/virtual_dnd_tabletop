#include "core/editor_paint.h"

#include <cstdlib>

namespace dnd {

auto LineCells(Cell from, Cell to) -> std::vector<Cell> {
  std::vector<Cell> cells;
  const int dx = std::abs(to.x - from.x);
  const int dy = -std::abs(to.y - from.y);
  const int step_x = from.x < to.x ? 1 : -1;
  const int step_y = from.y < to.y ? 1 : -1;
  int error = dx + dy;
  Cell cell = from;
  while (true) {
    cells.push_back(cell);
    if (cell == to) {
      break;
    }
    const int doubled = 2 * error;
    if (doubled >= dy) {
      error += dy;
      cell.x += step_x;
    }
    if (doubled <= dx) {
      error += dx;
      cell.y += step_y;
    }
  }
  return cells;
}

auto PaintDrag::Begin(Cell cell) -> void {
  active_ = true;
  last_ = cell;
}

auto PaintDrag::Move(Cell cell) -> std::vector<Cell> {
  if (!active_) {
    return {};
  }
  if (cell == last_) {
    return {};
  }
  std::vector<Cell> cells = LineCells(last_, cell);
  last_ = cell;
  return cells;
}

auto PaintDrag::End() -> void { active_ = false; }

}  // namespace dnd
