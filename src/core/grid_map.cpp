#include "core/grid_map.h"

#include <queue>
#include <vector>

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

auto ReachableCells(const GridMap& map, Cell origin, int radius)
    -> std::vector<Cell> {
  if (radius < 0 || !map.Contains(origin) || map.IsBlocked(origin)) {
    return {};
  }
  const auto width = static_cast<std::size_t>(map.Width());
  const auto height = static_cast<std::size_t>(map.Height());
  std::vector<int> distance(width * height, -1);
  std::queue<Cell> queue;
  distance[static_cast<std::size_t>(origin.y) * width +
           static_cast<std::size_t>(origin.x)] = 0;
  queue.push(origin);

  std::vector<Cell> result;
  constexpr int kDirections[8][2] = {{-1, -1}, {0, -1}, {1, -1}, {-1, 0},
                                     {1, 0},   {-1, 1}, {0, 1},  {1, 1}};
  while (!queue.empty()) {
    const Cell cell = queue.front();
    queue.pop();
    result.push_back(cell);
    const int current = distance[static_cast<std::size_t>(cell.y) * width +
                                 static_cast<std::size_t>(cell.x)];
    if (current >= radius) {
      continue;
    }
    for (const auto& direction : kDirections) {
      const Cell next{.x = cell.x + direction[0], .y = cell.y + direction[1]};
      if (!map.Contains(next) || map.IsBlocked(next)) {
        continue;
      }
      if (direction[0] != 0 && direction[1] != 0) {
        const Cell horizontal{.x = cell.x + direction[0], .y = cell.y};
        const Cell vertical{.x = cell.x, .y = cell.y + direction[1]};
        if (map.IsBlocked(horizontal) && map.IsBlocked(vertical)) {
          continue;
        }
      }
      const auto index = static_cast<std::size_t>(next.y) * width +
                         static_cast<std::size_t>(next.x);
      if (distance[index] != -1) {
        continue;
      }
      distance[index] = current + 1;
      queue.push(next);
    }
  }
  return result;
}

}  // namespace dnd
