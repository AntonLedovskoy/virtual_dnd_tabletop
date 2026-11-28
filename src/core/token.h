#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "core/grid_map.h"
#include "core/image_name.h"

namespace dnd {

using TokenId = std::uint32_t;

// Токены персонажей дают обзор партии. NPC и монстры видны только там,
// где игрок уже может видеть. Тип также определяет, каких токенов игроки
// могут забирать как своих персонажей.
enum class TokenKind : std::uint8_t {
  kPlayer = 0,
  kNpc = 1,
  kMonster = 2,
};

// Состояние токена. seconds > 0 — сколько игровых секунд ещё действует,
// 0 — до ручного снятия. save_dc > 0 — в конце хода токена бросается
// спасбросок против этой КС, успех снимает состояние; 0 — без спасброска.
struct Condition {
  std::string name;
  int seconds = 0;
  int save_dc = 0;
  // Источник состояния: 0 — его нет. «Очарован» запрещает атаковать
  // источник, «испуган» — приближаться к нему (2024).
  TokenId source_id = 0;

  Condition() = default;
  explicit Condition(std::string condition_name, int remaining = 0, int dc = 0,
                     TokenId source = 0)
      : name(std::move(condition_name)),
        seconds(remaining),
        save_dc(dc),
        source_id(source) {}
  explicit Condition(const char* condition_name, int remaining = 0, int dc = 0,
                     TokenId source = 0)
      : name(condition_name),
        seconds(remaining),
        save_dc(dc),
        source_id(source) {}
};

struct Token {
  TokenId id = 0;
  std::string name;
  // Пустой owner бывает только у ещё не занятого персонажа: такой токен
  // никто из игроков не двигает, пока его не заберут. У монстров и NPC
  // владелец всегда DM (core/player_name.h, kDmOwner).
  std::string owner;
  int hp = 10;
  int max_hp = 10;
  int temp_hp = 0;
  // Кость хитов (d6/d8/d10/d12) и модификатор Телосложения: по ним считается
  // прирост HP при повышении уровня (core/levels.h).
  int hit_die = 8;
  int con_modifier = 0;
  // Максимум HP считается по костям (core/hit_points): у персонажа — по
  // уровню, у монстра — по hit_dice_count. Старые файлы читаются без авто и
  // не пересчитываются.
  bool auto_hp = false;
  // Число костей хитов монстра; 0 — «по уровню», так у персонажей.
  int hit_dice_count = 0;
  // Потраченные кости хитов: запас равен уровню (core/levels.h), короткий
  // отдых их тратит, длинный возвращает все.
  int hit_dice_spent = 0;
  int initiative = 0;
  int ac = 10;
  int speed = 9;
  // Скорости плавания и лазания в клетках; 0 — их нет, и клетка всё равно
  // проходима, но дороже (core/grid_map, MovementProfile).
  int swim_speed = 0;
  int climb_speed = 0;
  // Тёмное зрение в клетках (в листе — метры); 0 — его нет. Свет, который
  // носит токен (факел, фонарь): радиусы яркого и тусклого света в клетках.
  int darkvision_cells = 0;
  int light_bright_cells = 0;
  int light_dim_cells = 0;
  std::string notes;
  int attack_bonus = 0;
  int save_bonus = 0;
  int initiative_bonus = 0;
  // Общий модификатор проверок способностей без бонуса мастерства; владения
  // навыками (маска core/skills.h) добавляют бонус мастерства по уровню.
  int skill_bonus = 0;
  std::uint32_t skill_proficiency = 0;
  // «1d8+2 огонь»: выражение урона и необязательный тип в конце (core/damage).
  std::string damage;
  // Защита от урона по типам: битовые маски core/damage.h.
  std::uint32_t resist = 0;
  std::uint32_t vulnerable = 0;
  std::uint32_t immune = 0;
  // Обычная дистанция атаки в клетках; 0 — ближний бой с досягаемостью
  // 1 клетка, больше нуля — дальнобойная атака (до двойной дистанции).
  int attack_range = 0;
  std::vector<Condition> conditions;
  // Накопленный опыт; уровень считается по нему (core/levels.h).
  int xp = 0;
  // Израсходованный за ход запас движения в клетках. Сбрасывается в начале
  // хода токена и вне боя не накапливается.
  int movement_used = 0;
  // Чётность диагоналей по правилу 5–10–5: 0 — следующая стоит 5 футов,
  // 1 — 10 футов. Сбрасывается вместе с movement_used.
  int diagonal_parity = 0;
  // Спасброски от смерти (0..3); имеют смысл только при hp == 0.
  int death_save_successes = 0;
  int death_save_failures = 0;
  TokenKind kind = TokenKind::kMonster;
  // 0xRRGGBB или 0, чтобы брать цвет из id токена.
  std::uint32_t color = 0;
  // Имя картинки-портрета рядом с .exe; пусто — цвет и инициалы.
  std::string portrait;
  Cell cell{};
  // Экономика хода 2024. Действие, бонусное действие и реакция сбрасываются
  // в начале хода токена. Рывок даёт movement_bonus клеток движения,
  // отход запрещает атаки по возможности, уклонение даёт помеху атакам по
  // токену; оба эффекта держатся до начала его следующего хода.
  bool action_used = false;
  bool bonus_action_used = false;
  bool reaction_used = false;
  int movement_bonus = 0;
  bool disengaged = false;
  bool dodging = false;
  // «Помощь» союзнику: преимущество на его следующую атаку или проверку.
  bool helped = false;
  // «Подготовиться»: действие отложено до срабатывания, реакция ещё свободна.
  bool readied = false;
  // Скрытность: успешная проверка Ловкости (Скрытность) прячет токен; пока
  // его пассивное Восприятие не перебьёт stealth_total, игроки токен не видят.
  bool hidden = false;
  int stealth_total = 0;
  // Сработавшие за этот ход профили урона карты (индексы в палитре): вход в
  // лаву не бьёт повторно. Очищается в начале хода токена и в конце боя.
  std::vector<std::uint8_t> hazards_done;
};

}  // namespace dnd
