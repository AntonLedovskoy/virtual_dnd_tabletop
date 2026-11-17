#include "core/grid_map.h"

namespace dnd {

GridMap::GridMap(int width, int height)
    : width_(width),
      height_(height),
      blocked_(static_cast<std::size_t>(width * height), 0) {}

auto GridMap::Contains(Cell cell) const -> bool {
  return cell.x >= 0 && cell.y >= 0 && cell.x < width_ && cell.y < height_;
}

auto GridMap::IsBlocked(Cell cell) const -> bool {
  return Contains(cell) && blocked_[Index(cell)] != 0;
}

auto GridMap::SetBlocked(Cell cell, bool blocked) -> void {
  if (!Contains(cell)) {
    return;
  }
  blocked_[Index(cell)] = blocked ? 1 : 0;
}

auto GridMap::Index(Cell cell) const -> std::size_t {
  return (static_cast<std::size_t>(cell.y) * static_cast<std::size_t>(width_)) +
         static_cast<std::size_t>(cell.x);
}

}  // namespace dnd
