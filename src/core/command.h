#pragma once

#include <cstdint>
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

// Только DM: переименовать токен.
struct RenameToken {
  TokenId id = 0;
  std::string name;
};

// Владелец или DM: изменить максимум HP; текущее HP ограничивается им.
struct SetTokenMaxHp {
  TokenId id = 0;
  int max_hp = 10;
};

// Только DM: задать цвет токена 0xRRGGBB (0 — автоматический цвет).
struct SetTokenColor {
  TokenId id = 0;
  std::uint32_t color = 0;
};

// Владелец или DM: картинка-портрет рядом с .exe; пусто — цвет и инициалы.
struct SetTokenPortrait {
  TokenId id = 0;
  std::string portrait;
};

// Предел модификатора Телосложения в листе персонажа.
inline constexpr int kMaxConModifier = 20;

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
  // Кость хитов (d6/d8/d10/d12) и модификатор Телосложения: по ним считается
  // прирост HP при повышении уровня.
  int hit_die = 8;
  int con_modifier = 0;
};

// Атака одним токеном другого: сервер бросает d20 + бонус атаки против КД
// цели и при попадании применяет урон из листа. 20 на d20 — критическое
// попадание с удвоенными костями, 1 — промах. advantage/disadvantage
// соответствуют броскам с преимуществом и помехой. reaction — атака по
// возможности вне своего хода: тратит реакцию и бьёт только соседнюю цель.
struct AttackToken {
  TokenId attacker = 0;
  TokenId target = 0;
  bool advantage = false;
  bool disadvantage = false;
  bool reaction = false;
};

// Действие хода по правилам 2024. Экономику ведёт сервер: действие и
// бонусное действие сбрасываются в начале хода токена, реакция — там же.
enum class TurnActionKind : std::uint8_t {
  kDash = 0,
  kDisengage = 1,
  kDodge = 2,
  kHelp = 3,
  kHide = 4,
  kSearch = 5,
  kStudy = 6,
  kUseObject = 7,
};

inline constexpr int kTurnActionCount = 8;

[[nodiscard]] constexpr auto IsValidTurnAction(TurnActionKind kind) -> bool {
  return static_cast<std::uint8_t>(kind) <
         static_cast<std::uint8_t>(kTurnActionCount);
}

// Владелец или DM: потратить действие на один из стандартных приёмов.
struct UseTurnAction {
  TokenId id = 0;
  TurnActionKind kind = TurnActionKind::kDash;
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

// Только DM: отдых партии. Короткий (1 час) тратит кости хитов персонажей на
// восстановление HP, длинный (8 часов) — все HP, кости хитов и состояния,
// снимает временные HP. Отдых двигает игровое время, поэтому срочные
// состояния истекают.
struct RestParty {
  bool long_rest = false;
};

// Только DM и только вне боя: прокрутить игровое время. Срочные состояния
// всех токенов уменьшаются на seconds, истёкшие снимаются.
struct AdvanceTime {
  int seconds = 0;
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

// Только DM: начислить опыт одному персонажу (id) или всей партии (party).
inline constexpr int kMaxXpAward = 100000;
struct AwardXp {
  TokenId id = 0;
  int amount = 0;
  bool party = false;
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

// Только DM: открыть столу круг клеток вокруг cell навсегда, закрыть такой
// же круг или снять все открытия. Радиус в клетках.
inline constexpr int kRevealRadiusCells = 4;
inline constexpr int kMaxRevealRadiusCells = 64;
struct RevealArea {
  Cell cell{};
  int radius = 0;
};
struct HideArea {
  Cell cell{};
  int radius = 0;
};
struct ClearRevealed {};

using Command =
    std::variant<MoveToken, Say, RollDice, RollExpression, AddToken,
                 RemoveToken, SetTokenHp, SetInitiative, ClaimToken,
                 ReleaseToken, SetTokenOwner, RenameToken, SetTokenMaxHp,
                 SetTokenColor, UpdateTokenSheet, UpdateTokenCombat,
                 StartEncounter, EndEncounter, NextTurn, SetAoe, AttackToken,
                 DamageToken, HealToken, RestParty, AdvanceTime, AoeDamage,
                 DeathSave, PingAt, SetMapBackground, SetTokenPortrait,
                 RevealArea, HideArea, ClearRevealed, AwardXp, UseTurnAction>;

}  // namespace dnd
