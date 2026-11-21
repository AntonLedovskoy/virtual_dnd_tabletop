#include "core/levels.h"

#include <algorithm>
#include <array>

namespace dnd {
namespace {

// Классическая таблица D&D 5e: порог опыта для каждого уровня.
constexpr std::array<int, kMaxLevel + 1> kXpThresholds{
    0,      0,      300,    900,    2700,   6500,   14000,
    23000,  34000,  48000,  64000,  85000,  100000, 120000,
    140000, 165000, 195000, 225000, 265000, 305000, 355000,
};

}  // namespace

auto XpForLevel(int level) -> int {
  return kXpThresholds[static_cast<std::size_t>(
      std::clamp(level, 1, kMaxLevel))];
}

auto LevelForXp(int xp) -> int {
  const int value = std::max(0, xp);
  int level = 1;
  for (int candidate = 2; candidate <= kMaxLevel; ++candidate) {
    if (value < XpForLevel(candidate)) {
      break;
    }
    level = candidate;
  }
  return level;
}

auto ProficiencyBonus(int level) -> int {
  return 2 + ((std::clamp(level, 1, kMaxLevel) - 1) / 4);
}

auto IsValidHitDie(int hit_die) -> bool {
  switch (hit_die) {
    case 6:
    case 8:
    case 10:
    case 12:
      return true;
    default:
      return false;
  }
}

auto HitDieAverage(int hit_die) -> int {
  // Неизвестная кость ведёт себя как d8 — безопасный запасной вариант;
  // допустимые кости проверяет IsValidHitDie.
  return IsValidHitDie(hit_die) ? (hit_die / 2) + 1 : 5;
}

auto HitPointsPerLevel(int hit_die, int con_modifier) -> int {
  return std::max(1, HitDieAverage(hit_die) + con_modifier);
}

auto HitDiceMaximum(int level) -> int {
  return std::clamp(level, 1, kMaxLevel);
}

auto HitDiceAvailable(int level, int spent) -> int {
  return std::max(0, HitDiceMaximum(level) - std::max(0, spent));
}

auto HitDieHeal(int roll, int con_modifier) -> int {
  return std::max(0, roll + con_modifier);
}

auto IsAbilityScoreLevel(int level) -> bool {
  return level == 4 || level == 8 || level == 12 || level == 16;
}

}  // namespace dnd
