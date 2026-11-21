#include "core/attack.h"

#include <string>
#include <utility>

namespace dnd {
namespace {

auto ModifierSuffix(int modifier) -> std::string {
  if (modifier > 0) {
    return " + " + std::to_string(modifier);
  }
  if (modifier < 0) {
    return " - " + std::to_string(-modifier);
  }
  return {};
}

}  // namespace

auto CheckAttack(int d20, int attack_bonus, int ac) -> AttackCheck {
  if (d20 == 20) {
    return AttackCheck{.hit = true, .critical = true};
  }
  if (d20 == 1) {
    return AttackCheck{};
  }
  return AttackCheck{.hit = d20 + attack_bonus >= ac};
}

auto RollAttackDamage(const DiceExpression& damage, bool critical,
                      std::mt19937& rng) -> DiceRollResult {
  if (!critical) {
    return RollDiceExpression(damage, rng);
  }
  // Модификатор входит в урон один раз, поэтому два броска идут без него.
  DiceExpression dice = damage;
  dice.modifier = 0;
  const DiceRollResult first = RollDiceExpression(dice, rng);
  const DiceRollResult second = RollDiceExpression(dice, rng);
  DiceRollResult result;
  result.rolls = first.rolls;
  result.rolls.insert(result.rolls.end(), second.rolls.begin(),
                      second.rolls.end());
  result.chosen = first.chosen + second.chosen;
  result.total = first.total + second.total + damage.modifier;
  result.breakdown = first.breakdown + " + " + second.breakdown +
                     ModifierSuffix(damage.modifier);
  return result;
}

}  // namespace dnd
