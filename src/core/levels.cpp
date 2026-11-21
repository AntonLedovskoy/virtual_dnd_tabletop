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

}  // namespace dnd
