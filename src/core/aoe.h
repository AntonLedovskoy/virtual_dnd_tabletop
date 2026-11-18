#pragma once

#include <cstdint>
#include <vector>

#include "core/grid_map.h"

namespace dnd {

// Вид шаблона области. kNone означает, что шаблон убран с карты.
enum class AoeShape : std::uint8_t {
  kNone = 0,
  kCircle = 1,
  kCone = 2,
  kLine = 3,
};

inline constexpr int kAoeDirections = 8;
inline constexpr int kMaxAoeRadius = 24;
inline constexpr int kMaxAoeWidth = 8;

// Шаблон области: источник, ориентация и размеры в клетках. Радиус круга и
// длина конуса/линии хранятся в radius, ширина линии — в width.
struct AoeTemplate {
  AoeShape shape = AoeShape::kNone;
  Cell origin{};
  int direction = 0;
  int radius = 2;
  int width = 1;

  friend auto operator==(const AoeTemplate&, const AoeTemplate&)
      -> bool = default;
};

// Единичное смещение для направления 0..7 (по часовой стрелке от востока).
[[nodiscard]] auto AoeDirectionOffset(int direction) -> Cell;

// Следующий вид по кругу: круг -> конус -> линия -> круг.
[[nodiscard]] auto NextAoeShape(AoeShape shape) -> AoeShape;

// Поворот и изменение размера с зажимом в допустимый диапазон.
[[nodiscard]] auto RotateAoe(const AoeTemplate& aoe, int steps) -> AoeTemplate;
[[nodiscard]] auto ResizeAoe(const AoeTemplate& aoe, int delta) -> AoeTemplate;
[[nodiscard]] auto WidenAoe(const AoeTemplate& aoe, int delta) -> AoeTemplate;

// Проверка значений перед сохранением: сервер не доверяет клиенту.
[[nodiscard]] auto IsValidAoe(const AoeTemplate& aoe) -> bool;

// Клетки карты, накрытые шаблоном. Стены не мешают области: шар и конус
// огибают углы, поэтому проверяются только границы карты.
[[nodiscard]] auto AoeCells(const GridMap& map, const AoeTemplate& aoe)
    -> std::vector<Cell>;

}  // namespace dnd
