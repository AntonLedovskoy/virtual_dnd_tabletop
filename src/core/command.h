#pragma once

#include <string>
#include <variant>

#include "core/aoe.h"
#include "core/encounter.h"
#include "core/grid_map.h"
#include "core/map_background.h"
#include "core/token.h"

namespace dnd {

// Намерения клиента, отправляемые серверу. Сервер проверяет их и превращает
// в события; команды никогда не меняют состояние клиента напрямую.
struct MoveToken {
  TokenId id = 0;
  Cell target{};
};

// Обычная строка чата, отображаемая в общем логе.
struct Say {
  std::string text;
};

// Запрос броска, например {count = 2, sides = 6} для 2d6. Бросается на
// сервере, чтобы все клиенты видели одинаковый результат.
struct RollDice {
  int count = 1;
  int sides = 20;
};

// Бросок выражения вида d20+5, 2d6+3 или adv d20+5. label — необязательная
// подпись в логе, например «Атака» или «Спасбросок».
struct RollExpression {
  std::string expression;
  std::string label;
};

// Только DM: создать токен (пустое имя заменяется автоматическим). Токены
// персонажей по умолчанию свободны, затем игрок может их забрать.
struct AddToken {
  std::string name;
  Cell cell{};
  std::string owner;
  TokenKind kind = TokenKind::kMonster;
  int max_hp = 10;
  int ac = 10;
  int speed = 9;
  std::string notes;
  int attack_bonus = 0;
  int save_bonus = 0;
  std::string damage;
};

// Только DM: удалить токен.
struct RemoveToken {
  TokenId id = 0;
};

// Владелец или DM: изменить токен.
struct SetTokenHp {
  TokenId id = 0;
  int hp = 0;
};

struct SetInitiative {
  TokenId id = 0;
  int value = 0;
};

// Игрок: забрать свободного персонажа. Сервер подставляет имя игрока.
struct ClaimToken {
  TokenId id = 0;
};

// Владелец: освободить своего персонажа, чтобы можно было взять другого.
struct ReleaseToken {
  TokenId id = 0;
};

// Только DM: назначить или очистить (пустой owner) владельца токена.
struct SetTokenOwner {
  TokenId id = 0;
  std::string owner;
};

// Владелец или DM: переименовать токен.
struct RenameToken {
  TokenId id = 0;
  std::string name;
};

// Владелец или DM: изменить максимум HP; текущее HP ограничивается им.
struct SetTokenMaxHp {
  TokenId id = 0;
  int max_hp = 10;
};

// Владелец или DM: задать цвет токена 0xRRGGBB (0 — автоматический цвет).
struct SetTokenColor {
  TokenId id = 0;
  std::uint32_t color = 0;
};

// Владелец или DM: редактировать лист персонажа или монстра в интерфейсе.
struct UpdateTokenSheet {
  TokenId id = 0;
  int ac = 10;
  int speed = 9;
  std::string notes;
  int attack_bonus = 0;
  int save_bonus = 0;
  int initiative_bonus = 0;
  std::string damage;
  int temp_hp = 0;
};

// Владелец или DM: нанести урон. Временные HP принимают удар первыми, при
// нуле HP сервер ставит состояние «без сознания».
struct DamageToken {
  TokenId id = 0;
  int amount = 1;
};

// Владелец или DM: вылечить токен, но не выше максимума HP.
struct HealToken {
  TokenId id = 0;
  int amount = 1;
};

// Только DM: отдых партии. Короткий восстанавливает половину максимума,
// длинный — все HP и снимает состояния и временные HP.
struct RestParty {
  bool long_rest = false;
};

// Только DM: урон по токенам под текущим шаблоном области. Спасбросок
// против save_dc уменьшает урон вдвое.
struct AoeDamage {
  std::string expression;
  int save_dc = 15;
};

// Владелец или DM: спасбросок от смерти у токена с нулём HP.
struct DeathSave {
  TokenId id = 0;
};

// Владелец или DM: заменить список состояний токена.
struct UpdateTokenCombat {
  TokenId id = 0;
  std::vector<Condition> conditions;
};

// Управление боем, доступное только DM.
struct StartEncounter {};
struct EndEncounter {};
struct NextTurn {};

// DM показывает столу шаблон области; kNone убирает его с карты.
struct SetAoe {
  AoeTemplate aoe;
};

// Любой участник ставит метку «сюда»: все, включая DM, видят её на карте
// несколько секунд. Метка не меняет мир и не сохраняется.
struct PingAt {
  Cell cell{};
};

// Только DM: картинка под сеткой. Пустое image убирает подложку.
struct SetMapBackground {
  MapBackground background;
};

using Command = std::variant<
    MoveToken, Say, RollDice, RollExpression, AddToken, RemoveToken, SetTokenHp,
    SetInitiative, ClaimToken, ReleaseToken, SetTokenOwner, RenameToken,
    SetTokenMaxHp, SetTokenColor, UpdateTokenSheet, UpdateTokenCombat,
    StartEncounter, EndEncounter, NextTurn, SetAoe, DamageToken, HealToken,
    RestParty, AoeDamage, DeathSave, PingAt, SetMapBackground>;

}  // namespace dnd
