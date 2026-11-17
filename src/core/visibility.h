#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "core/grid_map.h"

namespace dnd {

// Transferable form of a FogOfWar instance.
struct FogSnapshot {
  int width = 0;
  int height = 0;
  std::vector<std::uint8_t> visible;
  std::vector<std::uint8_t> explored;
};

// Per-viewer fog: what is currently in line of sight and what has ever been
// seen. The authoritative side keeps one instance per player.
class FogOfWar {
 public:
  FogOfWar(int width, int height);

  [[nodiscard]] auto Width() const -> int { return width_; }
  [[nodiscard]] auto Height() const -> int { return height_; }
  [[nodiscard]] auto IsVisible(Cell cell) const -> bool;
  [[nodiscard]] auto IsExplored(Cell cell) const -> bool;

  // Recomputes the visible set from the observers' positions and folds it into
  // the explored set. Observers outside the map are ignored.
  auto Recompute(const GridMap& map, const std::vector<Cell>& observers,
                 int radius) -> void;

  [[nodiscard]] auto Snapshot() const -> FogSnapshot;
  auto Restore(const FogSnapshot& snapshot) -> void;

 private:
  [[nodiscard]] auto Index(Cell cell) const -> std::size_t;

  int width_;
  int height_;
  std::vector<std::uint8_t> visible_;
  std::vector<std::uint8_t> explored_;
};

}  // namespace dnd
