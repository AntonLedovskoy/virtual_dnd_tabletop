#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace dnd {

struct Cell {
  int x = 0;
  int y = 0;

  friend auto operator==(const Cell&, const Cell&) -> bool = default;
};

class GridMap {
 public:
  GridMap(int width, int height);

  [[nodiscard]] auto Width() const -> int { return width_; }
  [[nodiscard]] auto Height() const -> int { return height_; }
  [[nodiscard]] auto Contains(Cell cell) const -> bool;
  [[nodiscard]] auto IsBlocked(Cell cell) const -> bool;

  auto SetBlocked(Cell cell, bool blocked) -> void;

 private:
  [[nodiscard]] auto Index(Cell cell) const -> std::size_t;

  int width_;
  int height_;
  std::vector<std::uint8_t> blocked_;
};

}  // namespace dnd
