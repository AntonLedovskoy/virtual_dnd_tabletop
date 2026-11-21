#include "core/grid_map.h"

#include <array>
#include <cstdint>
#include <functional>
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

// Дополнительные занятые клетки: токены, которые нельзя проходить насквозь.
auto BuildBlocked(const GridMap& map, std::span<const Cell> blocked)
    -> std::vector<std::uint8_t> {
  const auto width = static_cast<std::size_t>(map.Width());
  std::vector<std::uint8_t> extra(
      width * static_cast<std::size_t>(map.Height()), 0);
  for (const Cell cell : blocked) {
    if (!map.Contains(cell)) {
      continue;
    }
    extra[static_cast<std::size_t>(cell.y) * width +
          static_cast<std::size_t>(cell.x)] = 1;
  }
  return extra;
}

struct QueueEntry {
  int cost = 0;
  Cell cell{};
  int parity = 0;
};

struct Later {
  auto operator()(const QueueEntry& left, const QueueEntry& right) const
      -> bool {
    return left.cost > right.cost;
  }
};

// Стоимости достижения каждой клетки в двух состояниях чётности диагоналей.
// Индекс: (y * width + x) * 2 + parity, -1 — недостижимо.
auto RunPaths(const GridMap& map, Cell origin, int start_parity,
              const std::vector<std::uint8_t>& extra_blocked, int max_cost)
    -> std::vector<int> {
  const auto width = static_cast<std::size_t>(map.Width());
  const auto height = static_cast<std::size_t>(map.Height());
  std::vector<int> cost(width * height * 2, -1);
  if (!map.Contains(origin) || map.IsBlocked(origin)) {
    return cost;
  }
  const auto index = [width](Cell cell, int parity) -> std::size_t {
    return ((static_cast<std::size_t>(cell.y) * width) +
            static_cast<std::size_t>(cell.x)) *
               2 +
           static_cast<std::size_t>(parity);
  };
  const auto extra_blocked_at = [width, &extra_blocked](Cell cell) -> bool {
    return extra_blocked[static_cast<std::size_t>(cell.y) * width +
                         static_cast<std::size_t>(cell.x)] != 0;
  };

  const int parity = start_parity & 1;
  cost[index(origin, parity)] = 0;
  std::priority_queue<QueueEntry, std::vector<QueueEntry>, Later> queue;
  queue.push(QueueEntry{.cost = 0, .cell = origin, .parity = parity});

  while (!queue.empty()) {
    const QueueEntry current = queue.top();
    queue.pop();
    const std::size_t current_index = index(current.cell, current.parity);
    if (current.cost != cost[current_index]) {
      continue;
    }
    for (const Cell& direction : kNeighbours) {
      const Cell next{.x = current.cell.x + direction.x,
                      .y = current.cell.y + direction.y};
      if (!map.Contains(next) || map.IsBlocked(next) ||
          extra_blocked_at(next)) {
        continue;
      }
      const bool diagonal = direction.x != 0 && direction.y != 0;
      if (diagonal) {
        // Угол между двумя препятствиями не прорезаем: стены и занятые
        // токенами клетки считаются одинаково.
        const Cell horizontal{.x = current.cell.x + direction.x,
                              .y = current.cell.y};
        const Cell vertical{.x = current.cell.x,
                            .y = current.cell.y + direction.y};
        const auto blocked_at = [&map, &extra_blocked_at](Cell cell) -> bool {
          return map.IsBlocked(cell) || extra_blocked_at(cell);
        };
        if (blocked_at(horizontal) && blocked_at(vertical)) {
          continue;
        }
      }
      const int next_parity = diagonal ? 1 - current.parity : current.parity;
      // Опциональное правило 5–10–5: первая диагональ 5 футов, вторая 10.
      int step = 1;
      if (diagonal) {
        step = current.parity == 0 ? 1 : 2;
      }
      const int next_cost = current.cost + step;
      if (max_cost >= 0 && next_cost > max_cost) {
        continue;
      }
      const std::size_t next_index = index(next, next_parity);
      if (cost[next_index] != -1 && next_cost >= cost[next_index]) {
        continue;
      }
      cost[next_index] = next_cost;
      queue.push(
          QueueEntry{.cost = next_cost, .cell = next, .parity = next_parity});
    }
  }
  return cost;
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

auto ReachableCells(const GridMap& map, Cell origin, int budget,
                    int diagonal_parity, std::span<const Cell> blocked)
    -> std::vector<Cell> {
  if (budget < 0 || !map.Contains(origin) || map.IsBlocked(origin)) {
    return {};
  }
  const std::vector<std::uint8_t> extra = BuildBlocked(map, blocked);
  const std::vector<int> cost =
      RunPaths(map, origin, diagonal_parity, extra, budget);
  const auto width = static_cast<std::size_t>(map.Width());
  std::vector<Cell> result;
  for (int y = 0; y < map.Height(); ++y) {
    for (int x = 0; x < map.Width(); ++x) {
      const std::size_t base =
          (static_cast<std::size_t>(y) * width + static_cast<std::size_t>(x)) *
          2;
      if (cost[base] >= 0 || cost[base + 1] >= 0) {
        result.push_back(Cell{.x = x, .y = y});
      }
    }
  }
  return result;
}

auto PathCostCells(const GridMap& map, Cell from, Cell to, int diagonal_parity,
                   std::span<const Cell> blocked) -> std::optional<PathCost> {
  if (!map.Contains(from) || !map.Contains(to) || map.IsBlocked(to)) {
    return std::nullopt;
  }
  const std::vector<std::uint8_t> extra = BuildBlocked(map, blocked);
  const auto width = static_cast<std::size_t>(map.Width());
  const auto blocked_at = [&map, &extra, width](Cell cell) -> bool {
    return map.IsBlocked(cell) ||
           extra[static_cast<std::size_t>(cell.y) * width +
                 static_cast<std::size_t>(cell.x)] != 0;
  };
  if (blocked_at(to)) {
    return std::nullopt;
  }
  const int parity = diagonal_parity & 1;
  if (from == to) {
    return PathCost{.cost = 0, .diagonal_parity = parity};
  }
  const std::vector<int> cost = RunPaths(map, from, parity, extra, -1);
  const std::size_t base = (static_cast<std::size_t>(to.y) * width +
                            static_cast<std::size_t>(to.x)) *
                           2;
  std::optional<PathCost> best;
  // При равной стоимости выбираем чётность 0: следующая диагональ дешевле.
  for (int end_parity = 0; end_parity < 2; ++end_parity) {
    const int value = cost[base + static_cast<std::size_t>(end_parity)];
    if (value < 0) {
      continue;
    }
    if (!best.has_value() || value < best->cost) {
      best = PathCost{.cost = value, .diagonal_parity = end_parity};
    }
  }
  return best;
}

}  // namespace dnd
