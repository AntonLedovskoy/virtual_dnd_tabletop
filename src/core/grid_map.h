#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
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

// Результат прокладки пути по опциональному правилу 5–10–5: первая
// диагональ стоит 5 футов (1 клетку), вторая — 10 футов (2 клетки), затем
// снова 5 и так далее. Ортогональный шаг всегда стоит 1 клетку.
struct PathCost {
  int cost = 0;
  // Чётность диагоналей после пути: 0 — следующая диагональ стоит 5 футов,
  // 1 — 10 футов. Хранится в токене, чтобы счёт не сбрасывался между ходами.
  int diagonal_parity = 0;
};

// Клетки, достижимые из origin за budget (в тех же 5-футовых единицах).
// Стены, blocked и углы между двумя препятствиями непроходимы.
// diagonal_parity — сколько диагоналей уже пройдено в этом ходу (по модулю 2).
[[nodiscard]] auto ReachableCells(const GridMap& map, Cell origin, int budget,
                                  int diagonal_parity = 0,
                                  std::span<const Cell> blocked = {})
    -> std::vector<Cell>;

// Минимальная стоимость пути с учётом 5–10–5. Пусто, если клетка недостижима,
// за стеной, занята или путь срезал бы угол между двумя препятствиями.
[[nodiscard]] auto PathCostCells(const GridMap& map, Cell from, Cell to,
                                 int diagonal_parity = 0,
                                 std::span<const Cell> blocked = {})
    -> std::optional<PathCost>;

}  // namespace dnd
