#include "core/movement.h"

#include <algorithm>
#include <cstdlib>

#include "core/conditions.h"

namespace dnd {

auto IsProne(const Token& token) -> bool { return HasCondition(token, kProne); }

auto MovementProfileFor(const Token& token) -> MovementProfile {
  return MovementProfile{.speed = token.speed,
                         .swim_speed = token.swim_speed,
                         .climb_speed = token.climb_speed};
}

auto StandUpCostCells(const Token& token) -> int {
  if (!IsProne(token)) {
    return 0;
  }
  return std::max(1, EffectiveSpeedCells(token) / 2);
}

auto MovementBudgetCells(const Token& token) -> int {
  return EffectiveSpeedCells(token) + std::max(0, token.movement_bonus);
}

auto RemainingMovementCells(const Token& token) -> int {
  return std::max(0, MovementBudgetCells(token) - token.movement_used -
                         StandUpCostCells(token));
}

auto CrawlCostCells(int path_cost) -> int { return std::max(0, path_cost) * 2; }

auto JumpDistanceCells(Cell from, Cell to) -> int {
  const int dx = std::abs(to.x - from.x);
  const int dy = std::abs(to.y - from.y);
  if (dx == 0 && dy == 0) {
    return 0;
  }
  // Прыжок идёт по прямой: по строке, столбцу или диагонали.
  if (dx != 0 && dy != 0 && dx != dy) {
    return 0;
  }
  return std::max(dx, dy);
}

auto IsJumpPathClear(const GridMap& map, Cell from, Cell to) -> bool {
  const int dx = to.x - from.x;
  const int dy = to.y - from.y;
  const int steps = std::max(std::abs(dx), std::abs(dy));
  if (steps <= 1) {
    return true;
  }
  int step_x = 0;
  int step_y = 0;
  if (dx != 0) {
    step_x = dx > 0 ? 1 : -1;
  }
  if (dy != 0) {
    step_y = dy > 0 ? 1 : -1;
  }
  for (int i = 1; i < steps; ++i) {
    const Cell cell{.x = from.x + (step_x * i), .y = from.y + (step_y * i)};
    if (map.TerrainAt(cell) == Terrain::kWall) {
      return false;
    }
  }
  return true;
}

auto FallDamageDice(int feet) -> int {
  if (feet < kFallFeetPerDie) {
    return 0;
  }
  return std::min(kMaxFallDice, feet / kFallFeetPerDie);
}

}  // namespace dnd
