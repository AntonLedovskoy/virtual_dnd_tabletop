#include "core/aoe.h"

#include <algorithm>
#include <cmath>

namespace dnd {
namespace {

struct Direction {
  int dx = 0;
  int dy = 0;
};

constexpr Direction kDirections[kAoeDirections] = {
    {1, 0}, {1, 1}, {0, 1}, {-1, 1}, {-1, 0}, {-1, -1}, {0, -1}, {1, -1},
};

auto ClampDirection(int direction) -> int {
  return ((direction % kAoeDirections) + kAoeDirections) % kAoeDirections;
}

// Длинная сторона шаблона в клетках: круг — радиус, конус и линия — длина.
auto ClampRadius(int radius) -> int {
  return std::clamp(radius, 1, kMaxAoeRadius);
}

// Ширина линии всегда нечётная, чтобы ось шаблона шла по центру клетки.
auto ClampWidth(int width) -> int {
  const int clamped = std::clamp(width, 1, kMaxAoeWidth);
  return clamped % 2 == 0 ? clamped - 1 : clamped;
}

}  // namespace

auto AoeDirectionOffset(int direction) -> Cell {
  const Direction dir = kDirections[ClampDirection(direction)];
  return Cell{.x = dir.dx, .y = dir.dy};
}

auto NextAoeShape(AoeShape shape) -> AoeShape {
  switch (shape) {
    case AoeShape::kNone:
      return AoeShape::kCircle;
    case AoeShape::kCircle:
      return AoeShape::kCone;
    case AoeShape::kCone:
      return AoeShape::kLine;
    case AoeShape::kLine:
      return AoeShape::kCircle;
  }
  return AoeShape::kCircle;
}

auto RotateAoe(const AoeTemplate& aoe, int steps) -> AoeTemplate {
  AoeTemplate rotated = aoe;
  rotated.direction = ClampDirection(aoe.direction + steps);
  return rotated;
}

auto ResizeAoe(const AoeTemplate& aoe, int delta) -> AoeTemplate {
  AoeTemplate resized = aoe;
  resized.radius = ClampRadius(aoe.radius + delta);
  return resized;
}

auto WidenAoe(const AoeTemplate& aoe, int delta) -> AoeTemplate {
  AoeTemplate widened = aoe;
  widened.width = ClampWidth(aoe.width + (delta > 0 ? 2 : -2));
  return widened;
}

auto IsValidAoe(const AoeTemplate& aoe) -> bool {
  if (aoe.shape != AoeShape::kNone && aoe.shape != AoeShape::kCircle &&
      aoe.shape != AoeShape::kCone && aoe.shape != AoeShape::kLine) {
    return false;
  }
  if (aoe.direction < 0 || aoe.direction >= kAoeDirections) {
    return false;
  }
  if (aoe.radius < 1 || aoe.radius > kMaxAoeRadius) {
    return false;
  }
  if (aoe.width < 1 || aoe.width > kMaxAoeWidth || aoe.width % 2 == 0) {
    return false;
  }
  return true;
}

auto AoeCells(const GridMap& map, const AoeTemplate& aoe) -> std::vector<Cell> {
  std::vector<Cell> cells;
  if (aoe.shape == AoeShape::kNone || !IsValidAoe(aoe) ||
      !map.Contains(aoe.origin)) {
    return cells;
  }

  const Direction dir = kDirections[ClampDirection(aoe.direction)];
  const float length =
      std::sqrt(static_cast<float>(dir.dx * dir.dx + dir.dy * dir.dy));
  const float dir_x = static_cast<float>(dir.dx) / length;
  const float dir_y = static_cast<float>(dir.dy) / length;
  const float half_width = static_cast<float>((aoe.width - 1) / 2) + 0.75F;
  const float radius_squared = static_cast<float>(aoe.radius * aoe.radius);
  // Широкая линия выходит за радиус по поперечной оси, поэтому обход клеток
  // расширяем до полуширины.
  const int extent = std::max(aoe.radius, ((aoe.width - 1) / 2) + 1);

  for (int dy = -extent; dy <= extent; ++dy) {
    for (int dx = -extent; dx <= extent; ++dx) {
      const Cell cell{.x = aoe.origin.x + dx, .y = aoe.origin.y + dy};
      if (!map.Contains(cell)) {
        continue;
      }
      const float offset_x = static_cast<float>(dx);
      const float offset_y = static_cast<float>(dy);
      bool inside = false;
      switch (aoe.shape) {
        case AoeShape::kCircle:
          inside =
              (offset_x * offset_x) + (offset_y * offset_y) <= radius_squared;
          break;
        case AoeShape::kCone: {
          const float along = (offset_x * dir_x) + (offset_y * dir_y);
          const float lateral =
              std::abs((offset_x * dir_y) - (offset_y * dir_x));
          inside = along > 0.0F && along <= static_cast<float>(aoe.radius) &&
                   lateral <= along;
          break;
        }
        case AoeShape::kLine: {
          const float along = (offset_x * dir_x) + (offset_y * dir_y);
          const float lateral =
              std::abs((offset_x * dir_y) - (offset_y * dir_x));
          inside = along >= 0.0F && along <= static_cast<float>(aoe.radius) &&
                   lateral <= half_width;
          break;
        }
        case AoeShape::kNone:
          break;
      }
      if (inside) {
        cells.push_back(cell);
      }
    }
  }
  return cells;
}

}  // namespace dnd
