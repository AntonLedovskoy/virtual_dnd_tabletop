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

  Condition() = default;
  explicit Condition(std::string condition_name, int remaining = 0, int dc = 0)
      : name(std::move(condition_name)), seconds(remaining), save_dc(dc) {}
  explicit Condition(const char* condition_name, int remaining = 0, int dc = 0)
      : name(condition_name), seconds(remaining), save_dc(dc) {}
};

struct Token {
  TokenId id = 0;
  std::string name;
  // Пустой owner означает свободный/общий токен, который может двигать любой
  // игрок; DM всегда может двигать любой токен.
  std::string owner;
  int hp = 10;
  int max_hp = 10;
  int temp_hp = 0;
  // Кость хитов (d6/d8/d10/d12) и модификатор Телосложения: по ним считается
  // прирост HP при повышении уровня (core/levels.h).
  int hit_die = 8;
  int con_modifier = 0;
  // Потраченные кости хитов: запас равен уровню (core/levels.h), короткий
  // отдых их тратит, длинный возвращает все.
  int hit_dice_spent = 0;
  int initiative = 0;
  int ac = 10;
  int speed = 9;
  std::string notes;
  int attack_bonus = 0;
  int save_bonus = 0;
  int initiative_bonus = 0;
  std::string damage;
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
};

}  // namespace dnd
