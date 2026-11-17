#include "core/bestiary.h"

namespace dnd {

auto DefaultBestiary() -> const std::vector<MonsterTemplate>& {
  static const std::vector<MonsterTemplate> kBestiary{
      MonsterTemplate{
          .name = "Гоблин",
          .ac = 15,
          .max_hp = 7,
          .speed = 30,
          .notes = "Скимитар: +4, 1d6+2; короткий лук: +4, 1d6+2",
      },
      MonsterTemplate{
          .name = "Волк",
          .ac = 13,
          .max_hp = 11,
          .speed = 40,
          .notes = "Укус: +4, 2d4+2; тактика стаи",
      },
      MonsterTemplate{
          .name = "Бандит",
          .ac = 12,
          .max_hp = 11,
          .speed = 30,
          .notes = "Скимитар: +3, 1d6+1; лёгкий арбалет: +3, 1d8+1",
      },
      MonsterTemplate{
          .name = "Скелет",
          .ac = 13,
          .max_hp = 13,
          .speed = 30,
          .notes = "Короткий меч: +4, 1d6+2; уязвим к дробящему",
      },
      MonsterTemplate{
          .name = "Огр",
          .ac = 11,
          .max_hp = 59,
          .speed = 40,
          .notes = "Дубина: +6, 2d8+4; большой размер",
      },
  };
  return kBestiary;
}

}  // namespace dnd
