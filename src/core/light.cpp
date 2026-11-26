#include "core/light.h"

#include <algorithm>
#include <cstdlib>

#include "core/grid_map.h"
#include "core/units.h"

namespace dnd {
namespace {

// Луч света: стены режут, остальной рельеф прозрачен. Затемнение свет не
// останавливает — оно влияет на зрение, а не на сам источник.
auto LightPasses(const GridMap& map, Cell from, Cell to) -> bool {
  int x = from.x;
  int y = from.y;
  const int delta_x = std::abs(to.x - x);
  const int delta_y = -std::abs(to.y - y);
  const int step_x = x < to.x ? 1 : -1;
  const int step_y = y < to.y ? 1 : -1;
  int error = delta_x + delta_y;

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
    // Две стены, соприкасающиеся углом, не пропускают свет наискосок.
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
    if (map.BlocksSight(Cell{.x = x, .y = y})) {
      return false;
    }
  }
  return true;
}

}  // namespace

auto IsValidLightValue(std::uint8_t value) -> bool {
  return value <= kAmbientLight;
}

auto IsValidObscurement(std::uint8_t value) -> bool {
  return value <= static_cast<std::uint8_t>(Obscurement::kHeavy);
}

auto LightField::At(Cell cell) const -> LightLevel {
  if (cell.x < 0 || cell.y < 0 || cell.x >= width || cell.y >= height) {
    return LightLevel::kDark;
  }
  const auto value = levels[(static_cast<std::size_t>(cell.y) *
                             static_cast<std::size_t>(width)) +
                            static_cast<std::size_t>(cell.x)];
  return value <= static_cast<std::uint8_t>(LightLevel::kBright)
             ? static_cast<LightLevel>(value)
             : LightLevel::kBright;
}

auto ComputeLightField(const GridMap& map,
                       const std::vector<LightSource>& sources) -> LightField {
  LightField field{
      .width = map.Width(),
      .height = map.Height(),
      .levels = std::vector<std::uint8_t>(
          static_cast<std::size_t>(map.Width()) *
              static_cast<std::size_t>(map.Height()),
          static_cast<std::uint8_t>(map.AmbientLight())),
  };
  const auto index = [&field](Cell cell) -> std::size_t {
    return (static_cast<std::size_t>(cell.y) *
            static_cast<std::size_t>(field.width)) +
           static_cast<std::size_t>(cell.x);
  };
  // Клетка может задавать свой свет кистью редактора.
  for (int y = 0; y < field.height; ++y) {
    for (int x = 0; x < field.width; ++x) {
      const Cell cell{.x = x, .y = y};
      const std::uint8_t value = map.PropsAt(cell).light;
      if (value != kAmbientLight) {
        field.levels[index(cell)] = value;
      }
    }
  }
  const auto raise = [&field, &index](Cell cell, LightLevel level) -> void {
    const auto value = static_cast<std::uint8_t>(level);
    field.levels[index(cell)] = std::max(field.levels[index(cell)], value);
  };
  for (const LightSource& source : sources) {
    if (!map.Contains(source.cell)) {
      continue;
    }
    const int bright = std::clamp(source.bright_cells, 0, kMaxLightCells);
    const int dim = std::clamp(source.dim_cells, 0, kMaxLightCells);
    const int reach = std::max(bright, dim);
    if (reach <= 0) {
      continue;
    }
    // Внутри клетки источника всегда светло: сам факел видно и в темноте.
    raise(source.cell, bright > 0 ? LightLevel::kBright : LightLevel::kDim);
    const int reach_squared = reach * reach;
    const int bright_squared = bright * bright;
    for (int y = source.cell.y - reach; y <= source.cell.y + reach; ++y) {
      for (int x = source.cell.x - reach; x <= source.cell.x + reach; ++x) {
        const Cell cell{.x = x, .y = y};
        if (!map.Contains(cell) || cell == source.cell) {
          continue;
        }
        const int delta_x = x - source.cell.x;
        const int delta_y = y - source.cell.y;
        const int distance_squared = (delta_x * delta_x) + (delta_y * delta_y);
        if (distance_squared > reach_squared) {
          continue;
        }
        if (!LightPasses(map, source.cell, cell)) {
          continue;
        }
        if (distance_squared <= bright_squared) {
          raise(cell, LightLevel::kBright);
        } else {
          raise(cell, LightLevel::kDim);
        }
      }
    }
  }
  return field;
}

auto LightCellsFromMeters(int meters) -> int {
  if (meters <= 0) {
    return 0;
  }
  return static_cast<int>(
      std::lround(static_cast<float>(meters) / kMetersPerCell));
}

auto LightMetersFromCells(int cells) -> int {
  if (cells <= 0) {
    return 0;
  }
  return static_cast<int>(
      std::lround(static_cast<float>(cells) * kMetersPerCell));
}

}  // namespace dnd
