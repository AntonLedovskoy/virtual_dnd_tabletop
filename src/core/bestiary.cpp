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
          .id = "goblin",
          .name = "Гоблин",
          .ac = 15,
          .hit_die = 6,
          .con_modifier = 0,
          .hit_dice_count = 2,
          .speed = 9,
          .attack_bonus = 4,
          .save_bonus = 0,
          .damage = "1d6+2 рубящий",
          .notes = "Скимитар: +4, 1d6+2; короткий лук: +4, 1d6+2",
      },
      MonsterTemplate{
          .id = "wolf",
          .name = "Волк",
          .ac = 13,
          .hit_die = 8,
          .con_modifier = 1,
          .hit_dice_count = 2,
          .speed = 12,
          .attack_bonus = 4,
          .save_bonus = 0,
          .damage = "2d4+2 колющий",
          .notes = "Укус: +4, 2d4+2; тактика стаи",
      },
      MonsterTemplate{
          .id = "bandit",
          .name = "Бандит",
          .ac = 12,
          .hit_die = 8,
          .con_modifier = 1,
          .hit_dice_count = 2,
          .speed = 9,
          .attack_bonus = 3,
          .save_bonus = 0,
          .damage = "1d6+1 рубящий",
          .notes = "Скимитар: +3, 1d6+1; лёгкий арбалет: +3, 1d8+1",
      },
      MonsterTemplate{
          .id = "skeleton",
          .name = "Скелет",
          .ac = 13,
          .hit_die = 8,
          .con_modifier = 2,
          .hit_dice_count = 2,
          .speed = 9,
          .attack_bonus = 4,
          .save_bonus = 0,
          .damage = "1d6+2 рубящий",
          .notes = "Короткий меч: +4, 1d6+2; уязвим к дробящему",
          .vulnerable = DamageTypeBit(DamageType::kBludgeoning),
          .immune = DamageTypeBit(DamageType::kPoison),
      },
      MonsterTemplate{
          .id = "ogre",
          .name = "Огр",
          .ac = 11,
          .hit_die = 10,
          .con_modifier = 3,
          .hit_dice_count = 7,
          .speed = 12,
          .attack_bonus = 6,
          .save_bonus = 0,
          .damage = "2d8+4 дробящий",
          .notes = "Дубина: +6, 2d8+4; большой размер",
      },
      MonsterTemplate{
          .id = "zombie",
          .name = "Зомби",
          .ac = 8,
          .hit_die = 8,
          .con_modifier = 3,
          .hit_dice_count = 3,
          .speed = 6,
          .attack_bonus = 3,
          .save_bonus = 0,
          .damage = "1d6+1 дробящий",
          .notes = "Удар: +3, 1d6+1; стойкость нежити",
          .immune = DamageTypeBit(DamageType::kPoison),
      },
      MonsterTemplate{
          .id = "orc",
          .name = "Орк",
          .ac = 13,
          .hit_die = 8,
          .con_modifier = 3,
          .hit_dice_count = 2,
          .speed = 9,
          .attack_bonus = 5,
          .save_bonus = 0,
          .damage = "1d12+3 рубящий",
          .notes = "Секира: +5, 1d12+3; ярость при виде крови",
      },
      MonsterTemplate{
          .id = "kobold",
          .name = "Кобольд",
          .ac = 12,
          .hit_die = 6,
          .con_modifier = -1,
          .hit_dice_count = 2,
          .speed = 9,
          .attack_bonus = 4,
          .save_bonus = 0,
          .damage = "1d4+2 колющий",
          .notes = "Кинжал: +4, 1d4+2; тактика стаи, ловушки",
      },
      MonsterTemplate{
          .id = "giant-spider",
          .name = "Гигантский паук",
          .ac = 14,
          .hit_die = 10,
          .con_modifier = 1,
          .hit_dice_count = 4,
          .speed = 12,
          .attack_bonus = 5,
          .save_bonus = 0,
          .damage = "1d8+3 яд",
          .notes = "Укус: +5, 1d8+3 и яд; паутина, лазание",
      },
  };
  return kBestiary;
}

auto TokenFromTemplate(const MonsterTemplate& monster, TokenId id, Cell cell)
    -> Token {
  Token token;
  token.id = id;
  token.name = monster.name;
  token.owner = std::string(kDmOwner);
  token.kind = TokenKind::kMonster;
  token.hit_die = monster.hit_die;
  token.con_modifier = monster.con_modifier;
  token.hit_dice_count = monster.hit_dice_count;
  token.auto_hp = true;
  RecomputeAutoHp(token);
  token.ac = monster.ac;
  token.speed = monster.speed;
  token.notes = monster.notes;
  token.attack_bonus = monster.attack_bonus;
  token.save_bonus = monster.save_bonus;
  token.damage = monster.damage;
  token.attack_range = monster.attack_range;
  token.resist = monster.resist;
  token.vulnerable = monster.vulnerable;
  token.immune = monster.immune;
  token.cell = cell;
  return token;
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
