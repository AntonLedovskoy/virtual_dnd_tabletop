#pragma once

#include <cstdint>
#include <vector>

#include "core/cell.h"

namespace dnd {

// Освещённость. Порядок важен: поле света берёт максимум из нескольких
// источников, поэтому kBright > kDim > kDark.
enum class LightLevel : std::uint8_t {
  kDark = 0,
  kDim = 1,
  kBright = 2,
};

// Значение кисти «как освещённость сцены»: клетка не переопределяет свет.
inline constexpr std::uint8_t kAmbientLight = 3;

// Затемнение на клетке (SRD 5.2.1): тьма и дым дают сильное, тусклый свет и
// кустарник — слабое. Дерево на клетке тоже считается слабым затемнением.
enum class Obscurement : std::uint8_t {
  kNone = 0,
  kLight = 1,
  kHeavy = 2,
};

[[nodiscard]] auto IsValidLightValue(std::uint8_t value) -> bool;
[[nodiscard]] auto IsValidObscurement(std::uint8_t value) -> bool;

// Источник света на карте: клетка и радиусы в клетках. Яркий свет внутри
// bright_cells, тусклый — до dim_cells; дальше источник не достаёт.
struct LightSource {
  Cell cell{};
  int bright_cells = 0;
  int dim_cells = 0;
};

// До скольких клеток может доставать один источник. Ограничение защищает
// карту от бесконечных радиусов из файла.
inline constexpr int kMaxLightCells = 1000;

// Освещённость всех клеток карты. Хранится побайтно, как и свойства клеток.
struct LightField {
  int width = 0;
  int height = 0;
  std::vector<std::uint8_t> levels;

  [[nodiscard]] auto At(Cell cell) const -> LightLevel;
  [[nodiscard]] auto IsEmpty() const -> bool { return levels.empty(); }
};

class GridMap;

// Свет сцены: освещённость клеток из свойств карты и источников токенов.
// Клетка свойства light == kAmbientLight берёт ambient-свет сцены; источник
// светит по линии взгляда, стены его режут, затемнение — нет.
auto ComputeLightField(const GridMap& map,
                       const std::vector<LightSource>& sources) -> LightField;

// Метры из интерфейса (тёмное зрение, свет факела) в клетки: 1 клетка = 1,5 м.
[[nodiscard]] auto LightCellsFromMeters(int meters) -> int;
[[nodiscard]] auto LightMetersFromCells(int cells) -> int;

}  // namespace dnd
