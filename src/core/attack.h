#pragma once

#include <random>

#include "core/dice.h"

namespace dnd {

// Итог проверки атаки по правилам 2024: 20 на d20 всегда попадает и
// удваивает кости урона, 1 всегда промахивается.
struct AttackCheck {
  bool hit = false;
  bool critical = false;
};

auto CheckAttack(int d20, int attack_bonus, int ac) -> AttackCheck;

// Досягаемость атаки в клетках: ближний бой (0) — одна клетка, дальнобойная —
// двойная обычная дистанция (за ней — автоматический промах). Свойство Reach
// (10 футов) не моделируется.
[[nodiscard]] auto AttackReachCells(int attack_range) -> int;

[[nodiscard]] auto IsAttackInReach(int attack_range, int distance_cells)
    -> bool;

// Урон атаки: при критическом попадании кости бросаются дважды, а
// модификатор выражения прибавляется один раз.
auto RollAttackDamage(const DiceExpression& damage, bool critical,
                      std::mt19937& rng) -> DiceRollResult;

}  // namespace dnd
