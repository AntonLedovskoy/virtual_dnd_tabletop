#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "core/grid_map.h"
#include "core/light.h"

namespace dnd {

// Передаваемая форма экземпляра FogOfWar. light — освещённость видимых
// клеток для отрисовки; для невидимых она нулевая, чтобы не утекала разведка.
struct FogSnapshot {
  int width = 0;
  int height = 0;
  std::vector<std::uint8_t> visible;
  std::vector<std::uint8_t> explored;
  std::vector<std::uint8_t> light;
};

// Наблюдатель: своя клетка, радиус тёмного зрения и «ослеплён». Слепой
// наблюдатель не видит ничего; слепота проверяется сервером по состояниям.
struct Observer {
  Cell cell{};
  int darkvision_cells = 0;
  bool blinded = false;
};

// Туман для каждого наблюдателя: что сейчас в поле зрения и что когда-либо
// было увидено. Авторитетная сторона хранит по одному экземпляру на игрока.
class FogOfWar {
 public:
  FogOfWar(int width, int height);

  [[nodiscard]] auto Width() const -> int { return width_; }
  [[nodiscard]] auto Height() const -> int { return height_; }
  [[nodiscard]] auto IsVisible(Cell cell) const -> bool;
  [[nodiscard]] auto IsExplored(Cell cell) const -> bool;
  // Освещённость разведанной клетки; для невидимой — kDark.
  [[nodiscard]] auto LightAt(Cell cell) const -> LightLevel;

  // Пересчитывает видимое множество по наблюдателям, полю света и пределу
  // обзора. max_sight_cells <= 0 — без предела (игра), малый предел остаётся
  // для тестов. Наблюдатели вне карты игнорируются.
  auto Recompute(const GridMap& map, const std::vector<Observer>& observers,
                 const LightField& light, int max_sight_cells) -> void;

  // Совместимость: яркий свет, без тёмного зрения, радиус как предел обзора.
  auto Recompute(const GridMap& map, const std::vector<Cell>& observers,
                 int radius) -> void;

  // Клетки, которые DM открыл вручную: они видны и исследованы независимо
  // от наблюдателей. Вызывается после Recompute, который чистит visible.
  auto Reveal(const std::vector<Cell>& cells) -> void;

  // Точечная правка после FogChanged: клетка становится видимой/разведанной
  // ровно в том состоянии, что пришло от сервера. Клетки вне карты
  // игнорируются.
  auto SetCell(Cell cell, bool visible, bool explored,
               LightLevel light = LightLevel::kBright) -> void;

  [[nodiscard]] auto Snapshot() const -> FogSnapshot;
  auto Restore(const FogSnapshot& snapshot) -> void;

  // Исследованное множество без видимого: так туман хранится в сцене.
  [[nodiscard]] auto Explored() const -> const std::vector<std::uint8_t>& {
    return explored_;
  }

 private:
  [[nodiscard]] auto Index(Cell cell) const -> std::size_t;

  int width_;
  int height_;
  std::vector<std::uint8_t> visible_;
  std::vector<std::uint8_t> explored_;
  std::vector<std::uint8_t> light_;
};

}  // namespace dnd
