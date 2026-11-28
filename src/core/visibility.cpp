#include "core/visibility.h"

#include <algorithm>
#include <cstdlib>

namespace dnd {

// Линия взгляда по Брезенхему. Стены блокируют луч, но сама клетка стены
// остаётся видимой, чтобы игрок видел границу комнаты. Дерево обзор больше
// не режет (это слабое затемнение), вода, пропасть и лава взгляд не держат.
// Сильное затемнение обрывает луч, но его собственная клетка видна облаком.
// Диагональный шаг также проверяет двух ортогональных соседей: две стены,
// соприкасающиеся углом, не должны пропускать обзор.
auto HasLineOfSight(const GridMap& map, Cell from, Cell to) -> bool {
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
    if (map.BlocksSight(cell) ||
        map.ObscurementAt(cell) == Obscurement::kHeavy) {
      return false;
    }
  }
  return true;
}

namespace {

// Видна ли клетка одному наблюдателю при известном поле света.
auto Observes(const GridMap& map, const Observer& observer, Cell cell,
              const LightField& light, int max_sight_cells) -> bool {
  if (observer.blinded || !map.Contains(observer.cell)) {
    return false;
  }
  // Свою клетку персонаж знает и в полной тьме: иначе игрок потерял бы
  // собственный токен на карте.
  if (cell == observer.cell) {
    return true;
  }
  const int delta_x = cell.x - observer.cell.x;
  const int delta_y = cell.y - observer.cell.y;
  const int distance_squared = (delta_x * delta_x) + (delta_y * delta_y);
  if (max_sight_cells > 0 &&
      distance_squared > max_sight_cells * max_sight_cells) {
    return false;
  }
  if (!HasLineOfSight(map, observer.cell, cell)) {
    return false;
  }
  // Облако дыма видно само по себе, даже если за ним тьма.
  if (map.ObscurementAt(cell) == Obscurement::kHeavy) {
    return true;
  }
  if (light.At(cell) != LightLevel::kDark) {
    return true;
  }
  // В темноте видно только в пределах тёмного зрения.
  const int darkvision = observer.darkvision_cells;
  return darkvision > 0 && distance_squared <= darkvision * darkvision;
}

}  // namespace

FogOfWar::FogOfWar(int width, int height)
    : width_(width),
      height_(height),
      visible_(static_cast<std::size_t>(width * height), 0),
      explored_(visible_.size(), 0),
      light_(visible_.size(), 0) {}

auto FogOfWar::IsVisible(Cell cell) const -> bool {
  return cell.x >= 0 && cell.y >= 0 && cell.x < width_ && cell.y < height_ &&
         visible_[Index(cell)] != 0;
}

auto FogOfWar::IsExplored(Cell cell) const -> bool {
  return cell.x >= 0 && cell.y >= 0 && cell.x < width_ && cell.y < height_ &&
         explored_[Index(cell)] != 0;
}

auto FogOfWar::LightAt(Cell cell) const -> LightLevel {
  if (!IsVisible(cell)) {
    return LightLevel::kDark;
  }
  const auto value = light_[Index(cell)];
  return value <= static_cast<std::uint8_t>(LightLevel::kBright)
             ? static_cast<LightLevel>(value)
             : LightLevel::kBright;
}

auto FogOfWar::Recompute(const GridMap& map,
                         const std::vector<Observer>& observers,
                         const LightField& light, int max_sight_cells) -> void {
  std::ranges::fill(visible_, 0);
  std::ranges::fill(light_, 0);

  for (int y = 0; y < height_; ++y) {
    for (int x = 0; x < width_; ++x) {
      const Cell cell{.x = x, .y = y};
      for (const Observer& observer : observers) {
        if (!Observes(map, observer, cell, light, max_sight_cells)) {
          continue;
        }
        const std::size_t index = Index(cell);
        visible_[index] = 1;
        light_[index] = static_cast<std::uint8_t>(light.At(cell));
        break;
      }
    }
  }

  for (std::size_t i = 0; i < visible_.size(); ++i) {
    if (visible_[i] != 0) {
      explored_[i] = 1;
    }
  }
}

auto FogOfWar::Recompute(const GridMap& map, const std::vector<Cell>& observers,
                         int radius) -> void {
  std::vector<Observer> converted;
  converted.reserve(observers.size());
  for (const Cell cell : observers) {
    converted.push_back(Observer{.cell = cell});
  }
  const LightField bright{
      .width = map.Width(),
      .height = map.Height(),
      .levels = std::vector<std::uint8_t>(
          static_cast<std::size_t>(map.Width()) *
              static_cast<std::size_t>(map.Height()),
          static_cast<std::uint8_t>(LightLevel::kBright)),
  };
  Recompute(map, converted, bright, radius);
}

auto FogOfWar::Reveal(const std::vector<Cell>& cells) -> void {
  for (const Cell cell : cells) {
    if (cell.x < 0 || cell.y < 0 || cell.x >= width_ || cell.y >= height_) {
      continue;
    }
    visible_[Index(cell)] = 1;
    explored_[Index(cell)] = 1;
    // Открытая DM клетка не должна выглядеть тёмной: свет для неё неизвестен.
    light_[Index(cell)] = static_cast<std::uint8_t>(LightLevel::kBright);
  }
}

auto FogOfWar::SetCell(Cell cell, bool visible, bool explored, LightLevel light)
    -> void {
  if (cell.x < 0 || cell.y < 0 || cell.x >= width_ || cell.y >= height_) {
    return;
  }
  visible_[Index(cell)] = visible ? 1 : 0;
  explored_[Index(cell)] = explored ? 1 : 0;
  light_[Index(cell)] =
      visible ? static_cast<std::uint8_t>(light) : std::uint8_t{0};
}

auto FogOfWar::Snapshot() const -> FogSnapshot {
  std::vector<std::uint8_t> light = light_;
  // Для невидимых клеток свет не передаём: иначе клиент узнал бы, что там.
  for (std::size_t i = 0; i < light.size(); ++i) {
    if (visible_[i] == 0) {
      light[i] = 0;
    }
  }
  return FogSnapshot{
      .width = width_,
      .height = height_,
      .visible = visible_,
      .explored = explored_,
      .light = std::move(light),
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
  light_.assign(expected, 0);
  if (snapshot.visible.size() == expected) {
    visible_ = snapshot.visible;
  }
  if (snapshot.explored.size() == expected) {
    explored_ = snapshot.explored;
  }
  if (snapshot.light.size() == expected) {
    light_ = snapshot.light;
  }
}

auto FogOfWar::Index(Cell cell) const -> std::size_t {
  return (static_cast<std::size_t>(cell.y) * static_cast<std::size_t>(width_)) +
         static_cast<std::size_t>(cell.x);
}

}  // namespace dnd
