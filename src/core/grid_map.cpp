#include "core/grid_map.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <queue>
#include <utility>
#include <vector>

namespace dnd {
namespace {

constexpr std::array<Cell, 8> kNeighbours{{
    {-1, -1},
    {0, -1},
    {1, -1},
    {-1, 0},
    {1, 0},
    {-1, 1},
    {0, 1},
    {1, 1},
}};

// Смещение течения по часам от востока: индекс 1 — восток, 3 — юг.
constexpr std::array<Cell, 9> kCurrentOffsets{{
    {0, 0},
    {1, 0},
    {1, 1},
    {0, 1},
    {-1, 1},
    {-1, 0},
    {-1, -1},
    {0, -1},
    {1, -1},
}};

// Дополнительные занятые клетки: токены, которые нельзя проходить насквозь.
auto BuildBlocked(const GridMap& map, std::span<const Cell> blocked)
    -> std::vector<std::uint8_t> {
  const auto width = static_cast<std::size_t>(map.Width());
  std::vector<std::uint8_t> extra(
      width * static_cast<std::size_t>(map.Height()), 0);
  for (const Cell cell : blocked) {
    if (!map.Contains(cell)) {
      continue;
    }
    extra[static_cast<std::size_t>(cell.y) * width +
          static_cast<std::size_t>(cell.x)] = 1;
  }
  return extra;
}

auto BlocksMovement(Terrain terrain) -> bool {
  switch (terrain) {
    case Terrain::kWall:
    case Terrain::kPit:
      return true;
    case Terrain::kFloor:
    case Terrain::kTree:
    case Terrain::kWater:
    case Terrain::kLava:
    case Terrain::kSwamp:
    case Terrain::kMud:
    case Terrain::kSnow:
    case Terrain::kBrush:
    case Terrain::kLadder:
      return false;
  }
  return true;
}

struct QueueEntry {
  int cost = 0;
  Cell cell{};
  int parity = 0;
};

struct Later {
  auto operator()(const QueueEntry& left, const QueueEntry& right) const
      -> bool {
    return left.cost > right.cost;
  }
};

// Стоимости достижения каждой клетки в двух состояниях чётности диагоналей.
// Индекс: (y * width + x) * 2 + parity, -1 — недостижимо.
auto RunPaths(const GridMap& map, Cell origin, int start_parity,
              const std::vector<std::uint8_t>& extra_blocked, int max_cost,
              const MovementProfile& profile) -> std::vector<int> {
  const auto width = static_cast<std::size_t>(map.Width());
  const auto height = static_cast<std::size_t>(map.Height());
  std::vector<int> cost(width * height * 2, -1);
  if (!map.Contains(origin) || map.IsBlocked(origin)) {
    return cost;
  }
  const auto index = [width](Cell cell, int parity) -> std::size_t {
    return ((static_cast<std::size_t>(cell.y) * width) +
            static_cast<std::size_t>(cell.x)) *
               2 +
           static_cast<std::size_t>(parity);
  };
  const auto extra_blocked_at = [width, &extra_blocked](Cell cell) -> bool {
    return extra_blocked[static_cast<std::size_t>(cell.y) * width +
                         static_cast<std::size_t>(cell.x)] != 0;
  };

  const int parity = start_parity & 1;
  cost[index(origin, parity)] = 0;
  std::priority_queue<QueueEntry, std::vector<QueueEntry>, Later> queue;
  queue.push(QueueEntry{.cost = 0, .cell = origin, .parity = parity});

  while (!queue.empty()) {
    const QueueEntry current = queue.top();
    queue.pop();
    const std::size_t current_index = index(current.cell, current.parity);
    if (current.cost != cost[current_index]) {
      continue;
    }
    for (const Cell& direction : kNeighbours) {
      const Cell next{.x = current.cell.x + direction.x,
                      .y = current.cell.y + direction.y};
      if (!map.Contains(next) || map.IsBlocked(next) ||
          extra_blocked_at(next)) {
        continue;
      }
      const bool diagonal = direction.x != 0 && direction.y != 0;
      if (diagonal) {
        // Угол между двумя препятствиями не прорезаем: стены и занятые
        // токенами клетки считаются одинаково.
        const Cell horizontal{.x = current.cell.x + direction.x,
                              .y = current.cell.y};
        const Cell vertical{.x = current.cell.x,
                            .y = current.cell.y + direction.y};
        const auto blocked_at = [&map, &extra_blocked_at](Cell cell) -> bool {
          return map.IsBlocked(cell) || extra_blocked_at(cell);
        };
        if (blocked_at(horizontal) && blocked_at(vertical)) {
          continue;
        }
      }
      const int next_parity = diagonal ? 1 - current.parity : current.parity;
      // Свойства клетки (трудная местность, вода, лазание, дорога) дают
      // стоимость 1–2; правило 5–10–5 применяется поверх неё.
      const int cell_cost = map.MovementCost(next, profile);
      if (cell_cost < 0) {
        continue;
      }
      int step = cell_cost;
      if (diagonal && current.parity != 0) {
        step *= 2;
      }
      const int next_cost = current.cost + step;
      if (max_cost >= 0 && next_cost > max_cost) {
        continue;
      }
      const std::size_t next_index = index(next, next_parity);
      if (cost[next_index] != -1 && next_cost >= cost[next_index]) {
        continue;
      }
      cost[next_index] = next_cost;
      queue.push(
          QueueEntry{.cost = next_cost, .cell = next, .parity = next_parity});
    }
  }
  return cost;
}

}  // namespace

auto CoverOnSide(std::uint8_t sides, int side) -> CoverLevel {
  if (side < 0 || side >= kCoverSides) {
    return CoverLevel::kNone;
  }
  const int shift = side * 2;
  return static_cast<CoverLevel>((sides >> shift) & 0x3U);
}

auto SetCoverOnSide(std::uint8_t& sides, int side, CoverLevel level) -> void {
  if (side < 0 || side >= kCoverSides) {
    return;
  }
  const int shift = side * 2;
  sides = static_cast<std::uint8_t>(
      (sides & ~(0x3U << shift)) |
      ((static_cast<std::uint32_t>(level) & 0x3U) << shift));
}

auto SideTowards(Cell from, Cell to) -> int {
  // Сторона to, обращённая к from: оси экрана — x на восток, y на юг.
  const int dx = from.x - to.x;
  const int dy = from.y - to.y;
  if (std::abs(dx) >= std::abs(dy) && dx != 0) {
    return dx < 0 ? 3 : 1;  // from западнее — западная грань, иначе восточная.
  }
  if (dy != 0) {
    return dy < 0 ? 0 : 2;  // from севернее — северная грань.
  }
  return 0;
}

auto CoverBonus(CoverLevel level) -> int {
  switch (level) {
    case CoverLevel::kNone:
      return 0;
    case CoverLevel::kHalf:
      return 2;
    case CoverLevel::kThreeQuarters:
      return 5;
    case CoverLevel::kFull:
      return 0;
  }
  return 0;
}

auto CoverBetween(const GridMap& map, Cell attacker, Cell target)
    -> CoverLevel {
  if (attacker == target || !map.Contains(attacker) || !map.Contains(target)) {
    return CoverLevel::kNone;
  }
  // Сэмплируем луч с шагом в восьмую клетки: на дистанциях стола это дешевле
  // точного DDA, а грани клеток находятся точно.
  constexpr int samples_per_cell = 8;
  const int dx = target.x - attacker.x;
  const int dy = target.y - attacker.y;
  const int cells = std::max(std::abs(dx), std::abs(dy));
  const int samples = cells * samples_per_cell;
  const double start_x = static_cast<double>(attacker.x) + 0.5;
  const double start_y = static_cast<double>(attacker.y) + 0.5;
  auto best = CoverLevel::kNone;
  const auto consider = [&best](CoverLevel level) -> void {
    if (static_cast<std::uint8_t>(level) > static_cast<std::uint8_t>(best)) {
      best = level;
    }
  };
  Cell previous = attacker;
  for (int step = 1; step <= samples; ++step) {
    const double t = static_cast<double>(step) / static_cast<double>(samples);
    const int px =
        static_cast<int>(std::floor(start_x + (static_cast<double>(dx) * t)));
    const int py =
        static_cast<int>(std::floor(start_y + (static_cast<double>(dy) * t)));
    const Cell cell{.x = px, .y = py};
    if (cell == previous || !map.Contains(cell)) {
      continue;
    }
    const int side = SideTowards(previous, cell);
    consider(CoverOnSide(map.PropsAt(cell).cover_sides, side));
    // Та же грань со стороны предыдущей клетки: низкая стена может быть
    // записана на любой из двух клеток.
    const int opposite = (side + 2) % kCoverSides;
    consider(CoverOnSide(map.PropsAt(previous).cover_sides, opposite));
    if (cell != target && map.BlocksSight(cell)) {
      return CoverLevel::kFull;
    }
    previous = cell;
  }
  return best;
}

auto DefaultHazards() -> std::vector<HazardProfile> {
  std::vector<HazardProfile> hazards;
  hazards.push_back(HazardProfile{});
  // Лава по DMG 2024: 10d10 огня при входе и в начале хода.
  hazards.push_back(HazardProfile{.damage = "10d10",
                                  .type = DamageType::kFire,
                                  .triggers = kHazardEnter | kHazardTurnStart,
                                  .save_dc = 0,
                                  .half_on_success = true});
  hazards.push_back(HazardProfile{.damage = "2d6",
                                  .type = DamageType::kAcid,
                                  .triggers = kHazardEnter | kHazardTurnStart,
                                  .save_dc = 0,
                                  .half_on_success = true});
  hazards.push_back(HazardProfile{.damage = "1d10",
                                  .type = DamageType::kPiercing,
                                  .triggers = kHazardEnter,
                                  .save_dc = 0,
                                  .half_on_success = true});
  hazards.push_back(HazardProfile{.damage = "2d6",
                                  .type = DamageType::kFire,
                                  .triggers = kHazardEnter | kHazardTurnStart,
                                  .save_dc = 0,
                                  .half_on_success = true});
  return hazards;
}

auto IsValidHazardTriggers(std::uint8_t triggers) -> bool {
  return (triggers & ~kHazardAllTriggers) == 0;
}

auto CurrentDirectionOffset(std::uint8_t direction) -> Cell {
  if (direction >= kCurrentOffsets.size()) {
    return Cell{};
  }
  return kCurrentOffsets[direction];
}

GridMap::GridMap(int width, int height)
    : width_(width),
      height_(height),
      terrain_(static_cast<std::size_t>(width * height), 0),
      props_(static_cast<std::size_t>(width * height)),
      hazards_(DefaultHazards()) {}

auto GridMap::Contains(Cell cell) const -> bool {
  return cell.x >= 0 && cell.y >= 0 && cell.x < width_ && cell.y < height_;
}

auto GridMap::TerrainAt(Cell cell) const -> Terrain {
  if (!Contains(cell)) {
    return Terrain::kFloor;
  }
  const auto value = terrain_[Index(cell)];
  if (value > static_cast<std::uint8_t>(Terrain::kLadder)) {
    return Terrain::kFloor;
  }
  return static_cast<Terrain>(value);
}

auto GridMap::PropsAt(Cell cell) const -> const CellProps& {
  static const CellProps empty_props;
  if (!Contains(cell)) {
    return empty_props;
  }
  return props_[Index(cell)];
}

auto GridMap::HazardAt(Cell cell) const -> const HazardProfile* {
  if (!Contains(cell)) {
    return nullptr;
  }
  const std::size_t index = PropsAt(cell).hazard;
  if (index >= hazards_.size() || index == kNoHazard) {
    return nullptr;
  }
  return &hazards_[index];
}

auto GridMap::IsBlocked(Cell cell) const -> bool {
  return Contains(cell) && BlocksMovement(TerrainAt(cell));
}

auto GridMap::BlocksSight(Cell cell) const -> bool {
  return Contains(cell) && TerrainAt(cell) == Terrain::kWall;
}

auto GridMap::ObscurementAt(Cell cell) const -> Obscurement {
  if (!Contains(cell)) {
    return Obscurement::kNone;
  }
  const Obscurement stored = PropsAt(cell).obscurement;
  if (stored != Obscurement::kNone) {
    return stored;
  }
  // Крона дерева даёт слабое затемнение, но взгляд не режет.
  return TerrainAt(cell) == Terrain::kTree ? Obscurement::kLight
                                           : Obscurement::kNone;
}

auto GridMap::IsDifficultTerrain(Cell cell) const -> bool {
  if (!Contains(cell)) {
    return false;
  }
  switch (TerrainAt(cell)) {
    case Terrain::kTree:
    case Terrain::kSwamp:
    case Terrain::kMud:
    case Terrain::kSnow:
    case Terrain::kBrush:
      return true;
    default:
      return false;
  }
}

auto GridMap::MovementCost(Cell cell, const MovementProfile& profile) const
    -> int {
  if (!Contains(cell)) {
    return -1;
  }
  const Terrain terrain = TerrainAt(cell);
  if (BlocksMovement(terrain)) {
    return -1;
  }
  const CellProps& props = PropsAt(cell);
  if (terrain == Terrain::kLadder) {
    return 1;
  }
  // 2024: плавание и лазание добавляют 1 клетку, трудная местность — ещё 1;
  // дорога снимает только трудную местность. Скорость плавания или лазания
  // убирает свою надбавку.
  const bool deep_water =
      props.water == WaterDepth::kDeep || terrain == Terrain::kWater;
  const bool climbing = (props.flags & kCellClimbable) != 0;
  int cost = 1;
  if (deep_water && profile.swim_speed == 0) {
    ++cost;
  }
  if (climbing && profile.climb_speed == 0) {
    ++cost;
  }
  const bool road = (props.flags & kCellRoad) != 0;
  if (!road &&
      (IsDifficultTerrain(cell) || props.water == WaterDepth::kShallow)) {
    ++cost;
  }
  return cost;
}

auto GridMap::SetTerrain(Cell cell, Terrain terrain) -> void {
  if (!Contains(cell)) {
    return;
  }
  terrain_[Index(cell)] = static_cast<std::uint8_t>(terrain);
  ApplyTerrainProps(cell);
}

auto GridMap::SetBlocked(Cell cell, bool blocked) -> void {
  SetTerrain(cell, blocked ? Terrain::kWall : Terrain::kFloor);
}

auto GridMap::SetProps(Cell cell, CellProps props) -> void {
  if (!Contains(cell)) {
    return;
  }
  props_[Index(cell)] = props;
}

auto GridMap::SetHazards(std::vector<HazardProfile> hazards) -> void {
  if (hazards.empty()) {
    hazards = DefaultHazards();
  }
  hazards_ = std::move(hazards);
}

auto GridMap::ApplyTerrainProps(Cell cell) -> void {
  CellProps& props = props_[Index(cell)];
  const Terrain terrain = TerrainAt(cell);
  if (terrain == Terrain::kWater) {
    props.water = WaterDepth::kDeep;
  } else {
    props.water = WaterDepth::kDry;
  }
  if (terrain == Terrain::kLava) {
    if (props.hazard == kNoHazard) {
      props.hazard = kLavaHazard;
    }
  } else if (props.hazard == kLavaHazard) {
    props.hazard = kNoHazard;
  }
}

auto GridMap::Index(Cell cell) const -> std::size_t {
  return (static_cast<std::size_t>(cell.y) * static_cast<std::size_t>(width_)) +
         static_cast<std::size_t>(cell.x);
}

auto ReachableCells(const GridMap& map, Cell origin, int budget,
                    int diagonal_parity, std::span<const Cell> blocked,
                    const MovementProfile& profile) -> std::vector<Cell> {
  if (budget < 0 || !map.Contains(origin) || map.IsBlocked(origin)) {
    return {};
  }
  const std::vector<std::uint8_t> extra = BuildBlocked(map, blocked);
  const std::vector<int> cost =
      RunPaths(map, origin, diagonal_parity, extra, budget, profile);
  const auto width = static_cast<std::size_t>(map.Width());
  std::vector<Cell> result;
  for (int y = 0; y < map.Height(); ++y) {
    for (int x = 0; x < map.Width(); ++x) {
      const std::size_t base =
          (static_cast<std::size_t>(y) * width + static_cast<std::size_t>(x)) *
          2;
      if (cost[base] >= 0 || cost[base + 1] >= 0) {
        result.push_back(Cell{.x = x, .y = y});
      }
    }
  }
  return result;
}

auto PathCostCells(const GridMap& map, Cell from, Cell to, int diagonal_parity,
                   std::span<const Cell> blocked,
                   const MovementProfile& profile) -> std::optional<PathCost> {
  if (!map.Contains(from) || !map.Contains(to) || map.IsBlocked(to)) {
    return std::nullopt;
  }
  const std::vector<std::uint8_t> extra = BuildBlocked(map, blocked);
  const auto width = static_cast<std::size_t>(map.Width());
  const auto blocked_at = [&map, &extra, width](Cell cell) -> bool {
    return map.IsBlocked(cell) ||
           extra[static_cast<std::size_t>(cell.y) * width +
                 static_cast<std::size_t>(cell.x)] != 0;
  };
  if (blocked_at(to)) {
    return std::nullopt;
  }
  const int parity = diagonal_parity & 1;
  if (from == to) {
    return PathCost{.cost = 0, .diagonal_parity = parity};
  }
  const std::vector<int> cost = RunPaths(map, from, parity, extra, -1, profile);
  const std::size_t base = (static_cast<std::size_t>(to.y) * width +
                            static_cast<std::size_t>(to.x)) *
                           2;
  std::optional<PathCost> best;
  // При равной стоимости выбираем чётность 0: следующая диагональ дешевле.
  for (int end_parity = 0; end_parity < 2; ++end_parity) {
    const int value = cost[base + static_cast<std::size_t>(end_parity)];
    if (value < 0) {
      continue;
    }
    if (!best.has_value() || value < best->cost) {
      best = PathCost{.cost = value, .diagonal_parity = end_parity};
    }
  }
  return best;
}

}  // namespace dnd
