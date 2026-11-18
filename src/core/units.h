#pragma once

#include <cmath>
#include <string>

namespace dnd {

// Одна клетка сетки — 1,5 м. Скорости и расстояния в интерфейсе — в метрах.
inline constexpr float kMetersPerCell = 1.5F;

// Скорость в метрах -> бюджет движения в клетках за ход.
inline auto SpeedToCells(int meters) -> int {
  if (meters <= 0) {
    return 0;
  }
  return static_cast<int>(
      std::lround(static_cast<float>(meters) / kMetersPerCell));
}

// Клетки -> метры без дробных потерь: 3 клетки = 4,5 м.
inline auto FormatMeters(int cells) -> std::string {
  const int half_meters = cells * 3;
  if (half_meters % 2 == 0) {
    return std::to_string(half_meters / 2) + " м";
  }
  return std::to_string(half_meters / 2) + ",5 м";
}

}  // namespace dnd
