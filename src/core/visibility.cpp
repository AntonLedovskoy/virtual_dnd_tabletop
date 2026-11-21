#include "core/visibility.h"

#include <algorithm>
#include <cstdlib>
#include <limits>

namespace dnd {
namespace {

// Дерево не перекрывает взгляд полностью, но крона сокращает его: за
// клеткой дерева видно ещё две клетки (5e 2024: труднопроходимая
// растительность, сильное затемнение остаётся на усмотрение DM).
constexpr int kTreeSightReach = 2;

// Линия видимости по Брезенхему. Стены блокируют луч, но сама клетка стены
// остаётся видимой, чтобы игрок видел границу освещённой комнаты.
// Деревья сокращают обзор, вода, пропасть и лава взгляд не держат.
// Диагональный шаг также проверяет двух ортогональных соседей: две стены,
// соприкасающиеся углом, перекрывают проход и не должны пропускать обзор.
auto LineOfSightClear(const GridMap& map, Cell from, Cell to) -> bool {
  int x = from.x;
  int y = from.y;
  const int delta_x = std::abs(to.x - x);
  const int delta_y = -std::abs(to.y - y);
  const int step_x = x < to.x ? 1 : -1;
  const int step_y = y < to.y ? 1 : -1;
  int error = delta_x + delta_y;
  int steps = 0;
  int sight_limit = std::numeric_limits<int>::max();

  while (x != to.x || y != to.y) {
    const int doubled = 2 * error;
    const int previous_x = x;
    const int previous_y = y;
    if (doubled >= delta_y) {
      error += delta_y;
      x += step_x;
    }
    if (doubled <= delta_x) {
      error += delta_x;
      y += step_y;
    }
    ++steps;
    if (steps > sight_limit) {
      return false;
    }

    if (x != previous_x && y != previous_y) {
      const Cell horizontal{.x = x, .y = previous_y};
      const Cell vertical{.x = previous_x, .y = y};
      if (map.BlocksSight(horizontal) && map.BlocksSight(vertical)) {
        return false;
      }
    }

    if (x == to.x && y == to.y) {
      break;
    }
    const Cell cell{.x = x, .y = y};
    if (map.BlocksSight(cell)) {
      return false;
    }
    if (map.TerrainAt(cell) == Terrain::kTree) {
      sight_limit = std::min(sight_limit, steps + kTreeSightReach);
    }
  }
  return true;
}

}  // namespace

FogOfWar::FogOfWar(int width, int height)
    : width_(width),
      height_(height),
      visible_(static_cast<std::size_t>(width * height), 0),
      explored_(visible_.size(), 0) {}

auto FogOfWar::IsVisible(Cell cell) const -> bool {
  return cell.x >= 0 && cell.y >= 0 && cell.x < width_ && cell.y < height_ &&
         visible_[Index(cell)] != 0;
}

auto FogOfWar::IsExplored(Cell cell) const -> bool {
  return cell.x >= 0 && cell.y >= 0 && cell.x < width_ && cell.y < height_ &&
         explored_[Index(cell)] != 0;
}

auto FogOfWar::Recompute(const GridMap& map, const std::vector<Cell>& observers,
                         int radius) -> void {
  std::ranges::fill(visible_, 0);

  const int radius_squared = radius * radius;
  for (int y = 0; y < height_; ++y) {
    for (int x = 0; x < width_; ++x) {
      const Cell cell{.x = x, .y = y};
      for (const Cell observer : observers) {
        if (!map.Contains(observer)) {
          continue;
        }
        const int delta_x = x - observer.x;
        const int delta_y = y - observer.y;
        if ((delta_x * delta_x) + (delta_y * delta_y) > radius_squared) {
          continue;
        }
        if (LineOfSightClear(map, observer, cell)) {
          visible_[Index(cell)] = 1;
          break;
        }
      }
    }
  }

  for (std::size_t i = 0; i < visible_.size(); ++i) {
    if (visible_[i] != 0) {
      explored_[i] = 1;
    }
  }
}

auto FogOfWar::Reveal(const std::vector<Cell>& cells) -> void {
  for (const Cell cell : cells) {
    if (cell.x < 0 || cell.y < 0 || cell.x >= width_ || cell.y >= height_) {
      continue;
    }
    visible_[Index(cell)] = 1;
    explored_[Index(cell)] = 1;
  }
}

auto FogOfWar::SetCell(Cell cell, bool visible, bool explored) -> void {
  if (cell.x < 0 || cell.y < 0 || cell.x >= width_ || cell.y >= height_) {
    return;
  }
  visible_[Index(cell)] = visible ? 1 : 0;
  explored_[Index(cell)] = explored ? 1 : 0;
}

auto FogOfWar::Snapshot() const -> FogSnapshot {
  return FogSnapshot{
      .width = width_,
      .height = height_,
      .visible = visible_,
      .explored = explored_,
  };
}

auto FogOfWar::Restore(const FogSnapshot& snapshot) -> void {
  if (snapshot.width <= 0 || snapshot.height <= 0) {
    return;
  }
  width_ = snapshot.width;
  height_ = snapshot.height;
  const auto expected =
      static_cast<std::size_t>(width_) * static_cast<std::size_t>(height_);
  visible_.assign(expected, 0);
  explored_.assign(expected, 0);
  if (snapshot.visible.size() == expected) {
    visible_ = snapshot.visible;
  }
  if (snapshot.explored.size() == expected) {
    explored_ = snapshot.explored;
  }
}

auto FogOfWar::Index(Cell cell) const -> std::size_t {
  return (static_cast<std::size_t>(cell.y) * static_cast<std::size_t>(width_)) +
         static_cast<std::size_t>(cell.x);
}

}  // namespace dnd
