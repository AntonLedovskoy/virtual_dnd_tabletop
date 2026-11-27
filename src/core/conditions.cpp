#include "core/conditions.h"

#include <algorithm>
#include <cstdlib>

#include "core/light.h"
#include "core/movement.h"
#include "core/units.h"

namespace dnd {
namespace {

auto Plural(int count, std::string_view one, std::string_view few,
            std::string_view many) -> std::string_view {
  const int last_two = count % 100;
  const int last = count % 10;
  if (last_two >= 11 && last_two <= 14) {
    return many;
  }
  if (last == 1) {
    return one;
  }
  if (last >= 2 && last <= 4) {
    return few;
  }
  return many;
}

auto FormatUnit(int count, std::string_view one, std::string_view few,
                std::string_view many) -> std::string {
  return std::to_string(count) + " " +
         std::string(Plural(count, one, few, many));
}

}  // namespace

auto FormatConditionDuration(int seconds) -> std::string {
  if (seconds <= 0) {
    return "до снятия";
  }
  if (seconds % kSecondsPerHour == 0) {
    return FormatUnit(seconds / kSecondsPerHour, "час", "часа", "часов");
  }
  if (seconds % kSecondsPerMinute == 0) {
    return FormatUnit(seconds / kSecondsPerMinute, "минута", "минуты", "минут");
  }
  if (seconds % kSecondsPerRound == 0) {
    return FormatUnit(seconds / kSecondsPerRound, "ход", "хода", "ходов");
  }
  return FormatUnit(seconds, "секунда", "секунды", "секунд");
}

auto ConcentrationDc(int damage) -> int {
  return std::min(30, std::max(10, damage / 2));
}

auto IsIncapacitatingCondition(std::string_view name) -> bool {
  return name == kUnconscious || name == kIncapacitated || name == kDead ||
         name == "парализован" || name == "оглушён" || name == "окаменел";
}

auto IsIncapacitated(const Token& token) -> bool {
  if (token.hp == 0) {
    return true;
  }
  return std::ranges::any_of(token.conditions,
                             [](const Condition& condition) -> bool {
                               return IsIncapacitatingCondition(condition.name);
                             });
}

auto DefaultConditions() -> const std::vector<std::string>& {
  // Константа названа по стилю проекта (kCamelCase), хотя clang-tidy для
  // локальных констант ждёт lower_case.
  // NOLINTNEXTLINE(readability-identifier-naming)
  static const std::vector<std::string> kConditions{
      "ослеплён",
      "очарован",
      "оглох",
      "испуган",
      "захвачен",
      std::string(kIncapacitated),
      "невидим",
      "парализован",
      "окаменел",
      "отравлен",
      "сбит с ног",
      "опутан",
      "оглушён",
      std::string(kUnconscious),
      std::string(kExhaustion),
      std::string(kSurprised),
  };
  return kConditions;
}

auto HasCondition(const Token& token, std::string_view name) -> bool {
  return std::ranges::any_of(token.conditions,
                             [name](const Condition& condition) -> bool {
                               return condition.name == name;
                             });
}

auto ExhaustionLevel(const std::vector<Condition>& conditions) -> int {
  const auto count =
      std::ranges::count_if(conditions, [](const Condition& condition) -> bool {
        return condition.name == kExhaustion;
      });
  return std::min(kMaxExhaustionLevel, static_cast<int>(count));
}

auto ExhaustionLevel(const Token& token) -> int {
  return ExhaustionLevel(token.conditions);
}

auto ExhaustionPenalty(const Token& token) -> int {
  return 2 * ExhaustionLevel(token);
}

auto ExhaustionSpeedPenaltyCells(const Token& token) -> int {
  // В проекте клетка — 1,5 м, то есть 5 футов: штраф −5 футов за уровень.
  return ExhaustionLevel(token);
}

auto IsSpeedZeroed(const Token& token) -> bool {
  return HasCondition(token, "захвачен") || HasCondition(token, "опутан") ||
         HasCondition(token, "парализован") || HasCondition(token, "оглушён") ||
         HasCondition(token, "окаменел") || HasCondition(token, kUnconscious) ||
         HasCondition(token, kDead);
}

auto EffectiveSpeedCells(const Token& token) -> int {
  if (IsSpeedZeroed(token)) {
    return 0;
  }
  const int base =
      SpeedToCells(token.speed) - ExhaustionSpeedPenaltyCells(token);
  return std::max(0, base);
}

auto AttackRollModesFor(const Token& attacker, const Token& target,
                        int distance_cells, int attack_range_cells,
                        bool threatened, bool can_see_target,
                        bool target_can_see_attacker) -> AttackRollModes {
  AttackRollModes modes;
  const auto advantage = [&modes]() -> void { modes.advantage = true; };
  const auto disadvantage = [&modes]() -> void { modes.disadvantage = true; };

  // Атакующий: ослеплён, отравлен, испуган, сбит с ног, опутан.
  if (HasCondition(attacker, "ослеплён") ||
      HasCondition(attacker, "отравлен") || HasCondition(attacker, "испуган") ||
      HasCondition(attacker, kProne) || HasCondition(attacker, "опутан")) {
    disadvantage();
  }
  if (HasCondition(attacker, "невидим")) {
    advantage();
  }
  // Цель: ослеплена, невидима, сбита с ног, опутана, обездвижена.
  if (HasCondition(target, "ослеплён") || HasCondition(target, "опутан") ||
      HasCondition(target, "парализован") || HasCondition(target, "оглушён") ||
      HasCondition(target, "окаменел") || HasCondition(target, kUnconscious)) {
    advantage();
  }
  if (HasCondition(target, "невидим")) {
    disadvantage();
  }
  if (HasCondition(target, kProne)) {
    // Лежачего бьют с преимуществом только в 5 футах, иначе с помехой.
    if (distance_cells <= 1) {
      advantage();
    } else {
      disadvantage();
    }
  }
  if (target.dodging) {
    disadvantage();
  }
  // Сильное затемнение и тьма: цель не видна — помеха, атакующего не видно —
  // преимущество (2024 «эффективно ослеплён»). В одном облаке дыма или в
  // общей тьме обе поправки гасятся.
  if (!can_see_target) {
    disadvantage();
  }
  if (!target_can_see_attacker) {
    advantage();
  }
  // Дальнобойная атака: на дистанции и в 5 футах от другого противника.
  if (attack_range_cells > 0) {
    if (distance_cells > attack_range_cells || threatened) {
      disadvantage();
    }
  }
  return modes;
}

auto CanSeeForAttack(const GridMap& map, const LightField& light,
                     const Token& observer, Cell cell) -> bool {
  if (HasCondition(observer, kBlinded)) {
    return false;
  }
  // Сильное затемнение закрывает взгляд в обе стороны: из облака дыма не
  // видно наружу, снаружи не видно в облако.
  if (map.ObscurementAt(observer.cell) == Obscurement::kHeavy ||
      map.ObscurementAt(cell) == Obscurement::kHeavy) {
    return false;
  }
  if (light.At(cell) != LightLevel::kDark) {
    return true;
  }
  const int distance = std::max(std::abs(observer.cell.x - cell.x),
                                std::abs(observer.cell.y - cell.y));
  return distance <= observer.darkvision_cells;
}

auto IsAutoCriticalOnHit(const Token& target, int distance_cells) -> bool {
  if (distance_cells > 1) {
    return false;
  }
  return HasCondition(target, "парализован") ||
         HasCondition(target, kUnconscious);
}

auto EffectiveDamageProfile(const Token& target) -> DamageProfile {
  DamageProfile profile{.resist = target.resist,
                        .vulnerable = target.vulnerable,
                        .immune = target.immune};
  if (HasCondition(target, "окаменел")) {
    profile.resist |= kAllDamageTypes;
    profile.immune |= DamageTypeBit(DamageType::kPoison);
  }
  return profile;
}

auto IsSurprised(const Token& token) -> bool {
  return HasCondition(token, kSurprised);
}

auto InitiativeRollModesFor(const Token& token) -> AttackRollModes {
  AttackRollModes modes;
  if (HasCondition(token, "невидим")) {
    modes.advantage = true;
  }
  if (IsSurprised(token) || IsIncapacitated(token)) {
    modes.disadvantage = true;
  }
  return modes;
}

}  // namespace dnd
