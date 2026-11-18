#include "core/grid_map.h"

#include <array>
#include <queue>
#include <vector>

namespace dnd {
namespace {

constexpr std::array<Cell, 8> kNeighbours{{
    {-1, -1},
    {0, -1},
    {1, -1},
    {-1, 0},
    {1, 0},
    {-1, 1},
    {0, 1},
    {1, 1},
}};

// Расстояния в шагах от origin по восьми направлениям; -1 — недостижимо.
// max_steps < 0 означает поиск без ограничения дальности.
auto StepDistances(const GridMap& map, Cell origin, int max_steps)
    -> std::vector<int> {
  const auto width = static_cast<std::size_t>(map.Width());
  std::vector<int> distance(width * static_cast<std::size_t>(map.Height()), -1);
  if (!map.Contains(origin) || map.IsBlocked(origin)) {
    return distance;
  }
  std::queue<Cell> queue;
  distance[static_cast<std::size_t>(origin.y) * width +
           static_cast<std::size_t>(origin.x)] = 0;
  queue.push(origin);

  while (!queue.empty()) {
    const Cell cell = queue.front();
    queue.pop();
    const int current = distance[static_cast<std::size_t>(cell.y) * width +
                                 static_cast<std::size_t>(cell.x)];
    if (max_steps >= 0 && current >= max_steps) {
      continue;
    }
    for (const Cell& direction : kNeighbours) {
      const Cell next{.x = cell.x + direction.x, .y = cell.y + direction.y};
      if (!map.Contains(next) || map.IsBlocked(next)) {
        continue;
      }
      // Угол между двумя стенами не прорезаем.
      if (direction.x != 0 && direction.y != 0) {
        const Cell horizontal{.x = cell.x + direction.x, .y = cell.y};
        const Cell vertical{.x = cell.x, .y = cell.y + direction.y};
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
  return distance;
}

}  // namespace

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
  const std::vector<int> distance = StepDistances(map, origin, radius);
  const auto width = static_cast<std::size_t>(map.Width());
  std::vector<Cell> result;
  for (int y = 0; y < map.Height(); ++y) {
    for (int x = 0; x < map.Width(); ++x) {
      const auto index =
          static_cast<std::size_t>(y) * width + static_cast<std::size_t>(x);
      if (distance[index] >= 0) {
        result.push_back(Cell{.x = x, .y = y});
      }
    }
  }
  return result;
}

auto PathCostCells(const GridMap& map, Cell from, Cell to)
    -> std::optional<int> {
  if (!map.Contains(from) || !map.Contains(to) || map.IsBlocked(to)) {
    return std::nullopt;
  }
  if (from == to) {
    return 0;
  }
  const std::vector<int> distance = StepDistances(map, from, -1);
  const int steps = distance[static_cast<std::size_t>(to.y) *
                                 static_cast<std::size_t>(map.Width()) +
                             static_cast<std::size_t>(to.x)];
  if (steps < 0) {
    return std::nullopt;
  }
  return steps;
}

}  // namespace dnd
