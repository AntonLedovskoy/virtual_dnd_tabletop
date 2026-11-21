#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "core/grid_map.h"

namespace dnd {

// Передаваемая форма экземпляра FogOfWar.
struct FogSnapshot {
  int width = 0;
  int height = 0;
  std::vector<std::uint8_t> visible;
  std::vector<std::uint8_t> explored;
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

  // Пересчитывает видимое множество по позициям наблюдателей и добавляет его
  // в исследованное. Наблюдатели вне карты игнорируются.
  auto Recompute(const GridMap& map, const std::vector<Cell>& observers,
                 int radius) -> void;

  // Клетки, которые DM открыл вручную: они видны и исследованы независимо
  // от наблюдателей. Вызывается после Recompute, который чистит visible.
  auto Reveal(const std::vector<Cell>& cells) -> void;

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
};

}  // namespace dnd
