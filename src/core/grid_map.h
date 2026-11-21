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

// Рельеф клетки. Стена перекрывает и движение, и обзор; дерево проходимо,
// но стоит вдвое дороже и сокращает обзор; вода, пропасть и лава
// непроходимы, но не мешают стрелять и смотреть сквозь них.
enum class Terrain : std::uint8_t {
  kFloor = 0,
  kWall = 1,
  kTree = 2,
  kWater = 3,
  kPit = 4,
  kLava = 5,
};

class GridMap {
 public:
  GridMap(int width, int height);

  [[nodiscard]] auto Width() const -> int { return width_; }
  [[nodiscard]] auto Height() const -> int { return height_; }
  [[nodiscard]] auto Contains(Cell cell) const -> bool;
  [[nodiscard]] auto TerrainAt(Cell cell) const -> Terrain;
  // Непроходимый рельеф: стена, вода, пропасть и лава.
  [[nodiscard]] auto IsBlocked(Cell cell) const -> bool;
  // Перекрывает ли клетка линию обзора. Стена — да, остальной рельеф — нет;
  // деревья сокращают обзор отдельным правилом в visibility.cpp.
  [[nodiscard]] auto BlocksSight(Cell cell) const -> bool;
  // Клетка стоит двойного движения (2024: difficult terrain).
  [[nodiscard]] auto IsDifficultTerrain(Cell cell) const -> bool;

  auto SetTerrain(Cell cell, Terrain terrain) -> void;
  // Совместимость со старым API: true — стена, false — пол.
  auto SetBlocked(Cell cell, bool blocked) -> void;

 private:
  [[nodiscard]] auto Index(Cell cell) const -> std::size_t;

  int width_;
  int height_;
  std::vector<std::uint8_t> terrain_;
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
