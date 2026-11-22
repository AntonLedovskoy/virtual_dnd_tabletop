#include "core/bestiary.h"

#include <algorithm>
#include <cstdlib>
#include <utility>

namespace dnd {

auto DefaultBestiary() -> const std::vector<MonsterTemplate>& {
  // Константа названа по стилю проекта (kCamelCase), хотя clang-tidy для
  // локальных констант ждёт lower_case.
  // NOLINTNEXTLINE(readability-identifier-naming)
  static const std::vector<MonsterTemplate> kBestiary{
      MonsterTemplate{
          .name = "Гоблин",
          .ac = 15,
          .max_hp = 7,
          .speed = 9,
          .attack_bonus = 4,
          .save_bonus = 0,
          .damage = "1d6+2 рубящий",
          .notes = "Скимитар: +4, 1d6+2; короткий лук: +4, 1d6+2",
      },
      MonsterTemplate{
          .name = "Волк",
          .ac = 13,
          .max_hp = 11,
          .speed = 12,
          .attack_bonus = 4,
          .save_bonus = 0,
          .damage = "2d4+2 колющий",
          .notes = "Укус: +4, 2d4+2; тактика стаи",
      },
      MonsterTemplate{
          .name = "Бандит",
          .ac = 12,
          .max_hp = 11,
          .speed = 9,
          .attack_bonus = 3,
          .save_bonus = 0,
          .damage = "1d6+1 рубящий",
          .notes = "Скимитар: +3, 1d6+1; лёгкий арбалет: +3, 1d8+1",
      },
      MonsterTemplate{
          .name = "Скелет",
          .ac = 13,
          .max_hp = 13,
          .speed = 9,
          .attack_bonus = 4,
          .save_bonus = 0,
          .damage = "1d6+2 рубящий",
          .notes = "Короткий меч: +4, 1d6+2; уязвим к дробящему",
          .vulnerable = DamageTypeBit(DamageType::kBludgeoning),
          .immune = DamageTypeBit(DamageType::kPoison),
      },
      MonsterTemplate{
          .name = "Огр",
          .ac = 11,
          .max_hp = 59,
          .speed = 12,
          .attack_bonus = 6,
          .save_bonus = 0,
          .damage = "2d8+4 дробящий",
          .notes = "Дубина: +6, 2d8+4; большой размер",
      },
      MonsterTemplate{
          .name = "Зомби",
          .ac = 8,
          .max_hp = 22,
          .speed = 6,
          .attack_bonus = 3,
          .save_bonus = 0,
          .damage = "1d6+1 дробящий",
          .notes = "Удар: +3, 1d6+1; стойкость нежити",
          .immune = DamageTypeBit(DamageType::kPoison),
      },
      MonsterTemplate{
          .name = "Орк",
          .ac = 13,
          .max_hp = 15,
          .speed = 9,
          .attack_bonus = 5,
          .save_bonus = 0,
          .damage = "1d12+3 рубящий",
          .notes = "Секира: +5, 1d12+3; ярость при виде крови",
      },
      MonsterTemplate{
          .name = "Кобольд",
          .ac = 12,
          .max_hp = 5,
          .speed = 9,
          .attack_bonus = 4,
          .save_bonus = 0,
          .damage = "1d4+2 колющий",
          .notes = "Кинжал: +4, 1d4+2; тактика стаи, ловушки",
      },
      MonsterTemplate{
          .name = "Гигантский паук",
          .ac = 14,
          .max_hp = 26,
          .speed = 12,
          .attack_bonus = 5,
          .save_bonus = 0,
          .damage = "1d8+3 яд",
          .notes = "Укус: +5, 1d8+3 и яд; паутина, лазание",
      },
  };
  return kBestiary;
}

auto MonsterGroupSize(std::mt19937& rng) -> int {
  std::uniform_int_distribution<int> distribution(kMinMonsterGroup,
                                                  kMaxMonsterGroup);
  return distribution(rng);
}

auto MonsterGroupCells(const GridMap& map, Cell origin, int count,
                       const std::vector<Token>& tokens) -> std::vector<Cell> {
  std::vector<Cell> cells;
  if (count <= 0 || !map.Contains(origin) || map.IsBlocked(origin)) {
    return cells;
  }
  const auto occupied = [&tokens](Cell cell) -> bool {
    return std::ranges::any_of(tokens, [cell](const Token& token) -> bool {
      return token.cell == cell;
    });
  };
  if (occupied(origin)) {
    return cells;
  }
  cells.push_back(origin);

  const int max_radius = std::max(map.Width(), map.Height());
  for (int radius = 1;
       radius <= max_radius && std::cmp_less(cells.size(), count); ++radius) {
    // Кольцо вокруг origin: клетки дальней от центра границы квадрата.
    for (int y = origin.y - radius; y <= origin.y + radius; ++y) {
      for (int x = origin.x - radius; x <= origin.x + radius; ++x) {
        const Cell cell{.x = x, .y = y};
        if (!map.Contains(cell) || map.IsBlocked(cell) || occupied(cell)) {
          continue;
        }
        const int dx = std::abs(x - origin.x);
        const int dy = std::abs(y - origin.y);
        if (std::max(dx, dy) != radius) {
          continue;
        }
        cells.push_back(cell);
        if (std::cmp_greater_equal(cells.size(), count)) {
          return cells;
        }
      }
    }
  }
  return cells;
}

}  // namespace dnd
