#include "core/game_server.h"

#include <algorithm>
#include <cstdlib>
#include <iterator>
#include <optional>
#include <sstream>
#include <type_traits>
#include <utility>

#include "core/attack.h"
#include "core/clock.h"
#include "core/conditions.h"
#include "core/dice.h"
#include "core/levels.h"
#include "core/player_name.h"
#include "core/units.h"

namespace dnd {
namespace {

constexpr int kMaxDiceCount = 20;
constexpr int kMinDiceSides = 2;
constexpr int kMaxDiceSides = 100;
constexpr std::size_t kMaxChatLength = 256;
constexpr std::size_t kMaxTokenName = 64;
constexpr std::size_t kMaxNotes = 512;
constexpr std::size_t kMaxDamageExpression = 64;
constexpr std::size_t kMaxRollLabel = 64;
constexpr int kMaxHp = 1000000;
constexpr int kMaxAc = 1000;
constexpr int kMaxSpeed = 1000;
constexpr int kMaxBonus = 100;
constexpr int kMaxTempHp = 1000;
constexpr std::size_t kMaxConditions = 16;
constexpr std::size_t kMaxConditionLength = 32;

auto AuthorName(const std::string& actor) -> std::string {
  return actor.empty() ? "DM" : actor;
}

auto AppendIntChange(std::vector<std::string>& parts, const char* label,
                     int before, int after) -> void {
  if (before == after) {
    return;
  }
  parts.emplace_back(std::string(label) + " " + std::to_string(before) + "→" +
                     std::to_string(after));
}

auto JoinChanges(const std::vector<std::string>& parts) -> std::string {
  std::string text;
  for (const std::string& part : parts) {
    if (!text.empty()) {
      text += ", ";
    }
    text += part;
  }
  return text;
}

auto IsValidKind(TokenKind kind) -> bool {
  return kind == TokenKind::kPlayer || kind == TokenKind::kNpc ||
         kind == TokenKind::kMonster;
}

auto ControlRejectReason(const Token& token) -> const char* {
  if (token.kind == TokenKind::kPlayer && token.owner.empty()) {
    return "сначала заберите персонажа";
  }
  return "токен контролирует другой игрок";
}

// Токены, чей запас движения нужно показать клиентам после сброса.
auto TokensWithSpentMovement(const GameState& state) -> std::vector<TokenId> {
  std::vector<TokenId> ids;
  for (const Token& token : state.Tokens()) {
    if (token.movement_used != 0) {
      ids.push_back(token.id);
    }
  }
  return ids;
}

}  // namespace

GameServer::GameServer(const GameSnapshot& snapshot, int sight_radius,
                       std::uint32_t rng_seed)
    : state_(snapshot),
      party_fog_(state_.Map().Width(), state_.Map().Height()),
      sight_radius_(sight_radius),
      rng_(rng_seed) {
  RestoreFog(snapshot.fog);
  RecomputeFog();
}

auto GameServer::Reset(GameSnapshot snapshot) -> void {
  const std::vector<FogRecord> records = std::move(snapshot.fog);
  snapshot.fog.clear();
  state_ = GameState(std::move(snapshot));
  // Мир заменён: старые слепки вида описывают другую карту и токены.
  sent_views_.clear();
  RestoreFog(records);
  RecomputeFog();
}

auto GameServer::HandleCommand(const Command& command, const std::string& actor,
                               bool can_control_all) -> CommandOutcome {
  return std::visit(
      [&](const auto& cmd) -> CommandOutcome {
        using T = std::decay_t<decltype(cmd)>;
        if constexpr (std::is_same_v<T, MoveToken>) {
          return HandleMove(cmd, actor, can_control_all);
        } else if constexpr (std::is_same_v<T, Say>) {
          return HandleSay(cmd, actor);
        } else if constexpr (std::is_same_v<T, RollDice>) {
          return HandleRoll(cmd, actor);
        } else if constexpr (std::is_same_v<T, RollExpression>) {
          return HandleRollExpression(cmd, actor);
        } else if constexpr (std::is_same_v<T, AddToken>) {
          return HandleAddToken(cmd, can_control_all);
        } else if constexpr (std::is_same_v<T, RemoveToken>) {
          return HandleRemoveToken(cmd, can_control_all);
        } else if constexpr (std::is_same_v<T, SetTokenHp>) {
          return HandleSetHp(cmd, actor, can_control_all);
        } else if constexpr (std::is_same_v<T, SetInitiative>) {
          return HandleSetInitiative(cmd, actor, can_control_all);
        } else if constexpr (std::is_same_v<T, ClaimToken>) {
          return HandleClaimToken(cmd, actor);
        } else if constexpr (std::is_same_v<T, ReleaseToken>) {
          return HandleReleaseToken(cmd, actor);
        } else if constexpr (std::is_same_v<T, SetTokenOwner>) {
          return HandleSetTokenOwner(cmd, can_control_all);
        } else if constexpr (std::is_same_v<T, RenameToken>) {
          return HandleRenameToken(cmd, actor, can_control_all);
        } else if constexpr (std::is_same_v<T, SetTokenMaxHp>) {
          return HandleSetTokenMaxHp(cmd, actor, can_control_all);
        } else if constexpr (std::is_same_v<T, SetTokenColor>) {
          return HandleSetTokenColor(cmd, actor, can_control_all);
        } else if constexpr (std::is_same_v<T, SetTokenPortrait>) {
          return HandleSetTokenPortrait(cmd, actor, can_control_all);
        } else if constexpr (std::is_same_v<T, UpdateTokenSheet>) {
          return HandleUpdateTokenSheet(cmd, can_control_all);
        } else if constexpr (std::is_same_v<T, UpdateTokenCombat>) {
          return HandleUpdateTokenCombat(cmd, can_control_all);
        } else if constexpr (std::is_same_v<T, StartEncounter>) {
          return HandleStartEncounter(can_control_all);
        } else if constexpr (std::is_same_v<T, EndEncounter>) {
          return HandleEndEncounter(can_control_all);
        } else if constexpr (std::is_same_v<T, SetAoe>) {
          return HandleSetAoe(cmd, can_control_all);
        } else if constexpr (std::is_same_v<T, AttackToken>) {
          return HandleAttack(cmd, actor, can_control_all);
        } else if constexpr (std::is_same_v<T, DamageToken>) {
          return HandleDamage(cmd, actor, can_control_all);
        } else if constexpr (std::is_same_v<T, HealToken>) {
          return HandleHeal(cmd, actor, can_control_all);
        } else if constexpr (std::is_same_v<T, RestParty>) {
          return HandleRest(cmd, can_control_all);
        } else if constexpr (std::is_same_v<T, AdvanceTime>) {
          return HandleAdvanceTime(cmd, can_control_all);
        } else if constexpr (std::is_same_v<T, AoeDamage>) {
          return HandleAoeDamage(cmd, can_control_all);
        } else if constexpr (std::is_same_v<T, DeathSave>) {
          return HandleDeathSave(cmd, actor, can_control_all);
        } else if constexpr (std::is_same_v<T, PingAt>) {
          return HandlePing(cmd, actor);
        } else if constexpr (std::is_same_v<T, SetMapBackground>) {
          return HandleSetMapBackground(cmd, can_control_all);
        } else if constexpr (std::is_same_v<T, RevealArea>) {
          return HandleReveal(cmd, can_control_all);
        } else if constexpr (std::is_same_v<T, HideArea>) {
          return HandleHide(cmd, can_control_all);
        } else if constexpr (std::is_same_v<T, ClearRevealed>) {
          return HandleClearRevealed(can_control_all);
        } else if constexpr (std::is_same_v<T, AwardXp>) {
          return HandleAwardXp(cmd, can_control_all);
        } else {
          return HandleNextTurn(can_control_all);
        }
      },
      command);
}

auto GameServer::CanControl(const Token& token, const std::string& actor,
                            bool can_control_all) -> bool {
  if (can_control_all) {
    return true;
  }
  // Персонажи заблокированы, пока их кто-нибудь не заберёт. Свободных NPC
  // и общие токены по-прежнему может двигать любой игрок.
  if (token.kind == TokenKind::kPlayer && token.owner.empty()) {
    return false;
  }
  if (token.owner.empty()) {
    return true;
  }
  return SamePlayerName(token.owner, actor);
}

auto GameServer::HandleMove(const MoveToken& command, const std::string& actor,
                            bool can_control_all) -> CommandOutcome {
  const Token* token = FindToken(command.id);
  if (token == nullptr) {
    return CommandOutcome{.reject_reason = "токен не найден"};
  }
  if (!CanControl(*token, actor, can_control_all)) {
    return CommandOutcome{.reject_reason = ControlRejectReason(*token)};
  }
  // Игрок не может вслепую ткнуть в неразведанную клетку. DM двигает токены
  // свободно, а разведанная клетка остаётся допустимой, даже если сейчас
  // скрыта туманом.
  if (!can_control_all && !IsExploredTo(actor, command.target)) {
    return CommandOutcome{.reject_reason = "клетка не разведана"};
  }

  // Проверяем путь и для игрока вне боя: сквозь стены и монстров нельзя.
  // Свои персонажи друг друга не блокируют, но встать на занятую клетку
  // нельзя. DM по-прежнему двигает токены свободно.
  std::vector<Cell> blockers;
  bool target_occupied = false;
  for (const Token& other : state_.Tokens()) {
    if (other.id == command.id) {
      continue;
    }
    if (other.cell == command.target) {
      target_occupied = true;
    }
    if (other.kind != TokenKind::kPlayer) {
      blockers.push_back(other.cell);
    }
  }
  if (!can_control_all && target_occupied) {
    return CommandOutcome{.reject_reason = "клетка занята"};
  }

  const bool budgeted = state_.EncounterState().active && !can_control_all;
  std::optional<PathCost> path;
  if (!can_control_all) {
    path = PathCostCells(state_.Map(), token->cell, command.target,
                         token->diagonal_parity, blockers);
    if (!path.has_value()) {
      return CommandOutcome{.reject_reason = "нет пути к клетке"};
    }
  }
  if (budgeted) {
    if (token->movement_used + path->cost > SpeedToCells(token->speed)) {
      return CommandOutcome{.reject_reason = "не хватает движения на ход"};
    }
  }

  const std::optional<Event> event = state_.ApplyCommand(command);
  if (!event.has_value()) {
    return CommandOutcome{.reject_reason = "недопустимый ход"};
  }
  RecomputeFog();

  CommandOutcome outcome;
  outcome.accepted = true;
  outcome.events.emplace_back(*event);
  if (budgeted) {
    if (std::optional<Token> moved = state_.SpendMovement(
            command.id, path->cost, path->diagonal_parity)) {
      outcome.events.emplace_back(TokenUpdated{.token = std::move(*moved)});
    }
  }
  return outcome;
}

auto GameServer::HandleSay(const Say& command, const std::string& actor)
    -> CommandOutcome {
  if (command.text.empty() || command.text.size() > kMaxChatLength) {
    return CommandOutcome{.reject_reason = "некорректное сообщение"};
  }
  CommandOutcome outcome;
  outcome.accepted = true;
  outcome.events.emplace_back(LogLineAdded{
      .line = LogLine{.author = AuthorName(actor),
                      .text = command.text,
                      .timestamp = CurrentTimeSeconds()},
  });
  return outcome;
}

auto GameServer::HandleRoll(const RollDice& command, const std::string& actor)
    -> CommandOutcome {
  if (command.count < 1 || command.count > kMaxDiceCount ||
      command.sides < kMinDiceSides || command.sides > kMaxDiceSides) {
    return CommandOutcome{.reject_reason = "некорректные кубы"};
  }

  std::uniform_int_distribution<int> distribution(1, command.sides);
  std::ostringstream text;
  text << "rolls " << command.count << "d" << command.sides << ": ";
  int total = 0;
  for (int i = 0; i < command.count; ++i) {
    const int roll = distribution(rng_);
    total += roll;
    if (i > 0) {
      text << " + ";
    }
    text << roll;
  }
  text << " = " << total;

  CommandOutcome outcome;
  outcome.accepted = true;
  outcome.events.emplace_back(LogLineAdded{
      .line = LogLine{.author = AuthorName(actor),
                      .text = text.str(),
                      .timestamp = CurrentTimeSeconds()},
  });
  return outcome;
}

auto GameServer::HandleRollExpression(const RollExpression& command,
                                      const std::string& actor)
    -> CommandOutcome {
  DiceExpression expression;
  if (command.expression.empty() ||
      command.expression.size() > kMaxDamageExpression ||
      !ParseDiceExpression(command.expression, expression)) {
    return CommandOutcome{.reject_reason = "некорректное выражение кубов"};
  }
  if (command.label.size() > kMaxRollLabel) {
    return CommandOutcome{.reject_reason = "слишком длинная подпись броска"};
  }

  const DiceRollResult result = RollDiceExpression(expression, rng_);
  std::ostringstream text;
  if (!command.label.empty()) {
    text << command.label << ": ";
  }
  text << command.expression << " = " << result.breakdown;
  if (result.breakdown != std::to_string(result.total)) {
    text << " = " << result.total;
  }

  CommandOutcome outcome;
  outcome.accepted = true;
  outcome.events.emplace_back(LogLineAdded{
      .line = LogLine{.author = AuthorName(actor),
                      .text = text.str(),
                      .timestamp = CurrentTimeSeconds()},
  });
  return outcome;
}

auto GameServer::HandleAddToken(const AddToken& command, bool can_control_all)
    -> CommandOutcome {
  if (!can_control_all) {
    return CommandOutcome{.reject_reason = "токены добавляет только DM"};
  }
  if (!state_.Map().Contains(command.cell) ||
      state_.Map().IsBlocked(command.cell)) {
    return CommandOutcome{.reject_reason =
                              "нельзя создать токен в этой клетке"};
  }
  if (!IsValidKind(command.kind)) {
    return CommandOutcome{.reject_reason = "некорректный тип токена"};
  }
  std::string name = command.name;
  if (name.empty()) {
    name = command.kind == TokenKind::kPlayer ? "player" : "monster";
  }
  if (name.size() > kMaxTokenName) {
    return CommandOutcome{.reject_reason = "имя токена слишком длинное"};
  }
  if (command.max_hp <= 0 || command.max_hp > kMaxHp) {
    return CommandOutcome{.reject_reason = "некорректный максимум HP"};
  }
  if (command.ac <= 0 || command.ac > kMaxAc || command.speed < 0 ||
      command.speed > kMaxSpeed) {
    return CommandOutcome{.reject_reason = "некорректные КД или скорость"};
  }
  if (command.notes.size() > kMaxNotes) {
    return CommandOutcome{.reject_reason = "заметки слишком длинные"};
  }
  if (command.attack_bonus < -kMaxBonus || command.attack_bonus > kMaxBonus ||
      command.save_bonus < -kMaxBonus || command.save_bonus > kMaxBonus) {
    return CommandOutcome{.reject_reason = "некорректный бонус"};
  }
  if (command.damage.size() > kMaxDamageExpression) {
    return CommandOutcome{.reject_reason = "слишком длинное выражение урона"};
  }
  if (!command.damage.empty()) {
    DiceExpression damage;
    if (!ParseDiceExpression(command.damage, damage)) {
      return CommandOutcome{.reject_reason = "некорректное выражение урона"};
    }
  }
  std::string owner = command.owner;
  if (owner.size() > kMaxTokenName) {
    return CommandOutcome{.reject_reason = "имя владельца слишком длинное"};
  }
  if (owner.empty() && command.kind != TokenKind::kPlayer) {
    owner = "dm";
  }

  const bool encounter_active = state_.EncounterState().active;
  const TokenId id = state_.AddToken(
      name, command.cell, std::move(owner), command.kind, command.max_hp, 0,
      command.ac, command.speed, command.notes, command.attack_bonus,
      command.save_bonus, command.damage);
  const std::optional<Token> token = state_.TokenCopy(id);
  if (!token.has_value()) {
    return CommandOutcome{.reject_reason = "не удалось создать токен"};
  }
  CommandOutcome outcome;
  outcome.accepted = true;
  outcome.events.emplace_back(TokenAdded{.token = *token});
  if (encounter_active) {
    outcome.events.emplace_back(
        EncounterUpdated{.encounter = state_.EncounterState()});
  }
  if (command.kind == TokenKind::kPlayer) {
    // Новый персонаж может изменить обзор партии.
    RecomputeFog();
  }
  return outcome;
}

auto GameServer::HandleRemoveToken(const RemoveToken& command,
                                   bool can_control_all) -> CommandOutcome {
  if (!can_control_all) {
    return CommandOutcome{.reject_reason = "токены удаляет только DM"};
  }
  const Token* token = FindToken(command.id);
  if (token == nullptr) {
    return CommandOutcome{.reject_reason = "токен не найден"};
  }
  const bool was_player = token->kind == TokenKind::kPlayer;
  const bool encounter_active = state_.EncounterState().active;
  if (!state_.RemoveToken(command.id)) {
    return CommandOutcome{.reject_reason = "токен не найден"};
  }
  CommandOutcome outcome;
  outcome.accepted = true;
  outcome.events.emplace_back(TokenRemoved{.id = command.id});
  if (encounter_active) {
    outcome.events.emplace_back(
        EncounterUpdated{.encounter = state_.EncounterState()});
  }
  if (was_player) {
    RecomputeFog();
  }
  return outcome;
}

auto GameServer::HandleSetHp(const SetTokenHp& command,
                             const std::string& actor, bool can_control_all)
    -> CommandOutcome {
  const Token* token = FindToken(command.id);
  if (token == nullptr) {
    return CommandOutcome{.reject_reason = "токен не найден"};
  }
  if (!CanControl(*token, actor, can_control_all)) {
    return CommandOutcome{.reject_reason = ControlRejectReason(*token)};
  }
  const std::optional<Token> updated =
      state_.SetTokenHp(command.id, command.hp);
  if (!updated.has_value()) {
    return CommandOutcome{.reject_reason = "не удалось обновить токен"};
  }
  CommandOutcome outcome;
  outcome.accepted = true;
  outcome.events.emplace_back(TokenUpdated{.token = *updated});
  return outcome;
}

auto GameServer::HandleDamage(const DamageToken& command,
                              const std::string& actor, bool can_control_all)
    -> CommandOutcome {
  const Token* token = FindToken(command.id);
  if (token == nullptr) {
    return CommandOutcome{.reject_reason = "токен не найден"};
  }
  if (!CanControl(*token, actor, can_control_all)) {
    return CommandOutcome{.reject_reason = ControlRejectReason(*token)};
  }
  if (command.amount <= 0 || command.amount > kMaxHp) {
    return CommandOutcome{.reject_reason = "некорректный урон"};
  }
  CommandOutcome outcome;
  outcome.accepted = true;
  DamageAndLog(command.id, command.amount, actor, outcome.events);
  return outcome;
}

auto GameServer::DamageAndLog(TokenId id, int amount, const std::string& actor,
                              std::vector<Event>& events) -> void {
  const Token* token = FindToken(id);
  if (token == nullptr) {
    return;
  }
  const std::string name = token->name;
  const int temp_before = token->temp_hp;
  const int hp_before = token->hp;
  const int save_bonus = token->save_bonus;
  const bool concentrating = std::ranges::any_of(
      token->conditions, [](const Condition& condition) -> bool {
        return condition.name == kConcentration;
      });
  const std::optional<Token> updated = state_.DamageToken(id, amount);
  if (!updated.has_value()) {
    return;
  }
  const int absorbed = std::min(temp_before, amount);
  std::ostringstream text;
  text << name << " получил " << amount << " урона";
  if (absorbed > 0) {
    text << " (" << absorbed << " из временных HP)";
  }
  if (hp_before > 0 && updated->hp == 0) {
    text << "; " << name << " без сознания";
  }
  events.emplace_back(TokenUpdated{.token = *updated});
  events.emplace_back(LogLineAdded{
      .line = LogLine{.author = AuthorName(actor),
                      .text = text.str(),
                      .timestamp = CurrentTimeSeconds()},
  });
  if (!concentrating) {
    return;
  }
  // Проверка концентрации: d20 + бонус спасброска против 10 или половины
  // полученного урона. Провал снимает состояние «концентрация».
  const int dc = std::max(10, amount / 2);
  std::uniform_int_distribution<int> d20(1, 20);
  const int roll = d20(rng_) + save_bonus;
  std::ostringstream check;
  check << name << ": проверка концентрации " << roll << " против " << dc;
  if (roll < dc) {
    check << " — сорвана";
    if (std::optional<Token> without =
            state_.RemoveTokenCondition(id, kConcentration)) {
      events.emplace_back(TokenUpdated{.token = std::move(*without)});
    }
  } else {
    check << " — удержана";
  }
  events.emplace_back(LogLineAdded{
      .line = LogLine{.author = AuthorName(actor),
                      .text = check.str(),
                      .timestamp = CurrentTimeSeconds()},
  });
}

auto GameServer::HandleAttack(const AttackToken& command,
                              const std::string& actor, bool can_control_all)
    -> CommandOutcome {
  const Token* attacker = FindToken(command.attacker);
  if (attacker == nullptr) {
    return CommandOutcome{.reject_reason = "атакующий не найден"};
  }
  const Token* target = FindToken(command.target);
  if (target == nullptr) {
    return CommandOutcome{.reject_reason = "цель не найдена"};
  }
  if (attacker->id == target->id) {
    return CommandOutcome{.reject_reason = "нельзя атаковать себя"};
  }
  if (!CanControl(*attacker, actor, can_control_all)) {
    return CommandOutcome{.reject_reason = ControlRejectReason(*attacker)};
  }
  // Игрок не бьёт то, чего не видит: иначе командой вручную можно нащупать
  // монстра в тумане. DM видит весь стол.
  if (!can_control_all && !IsVisibleTo(actor, target->cell)) {
    return CommandOutcome{.reject_reason = "цель не видна"};
  }
  if (command.advantage && command.disadvantage) {
    return CommandOutcome{.reject_reason =
                              "нельзя одновременно преимущество и помеху"};
  }
  DiceExpression damage;
  if (attacker->damage.size() > kMaxDamageExpression ||
      (!attacker->damage.empty() &&
       !ParseDiceExpression(attacker->damage, damage))) {
    return CommandOutcome{.reject_reason = "некорректное выражение урона"};
  }

  const DiceExpression attack{
      .count = 1,
      .sides = 20,
      .modifier = attacker->attack_bonus,
      .advantage = command.advantage,
      .disadvantage = command.disadvantage,
  };
  const DiceRollResult roll = RollDiceExpression(attack, rng_);
  const AttackCheck check =
      CheckAttack(roll.chosen, attacker->attack_bonus, target->ac);

  std::ostringstream text;
  text << "Атака: " << attacker->name << " по " << target->name;
  if (command.advantage) {
    text << " с преимуществом";
  } else if (command.disadvantage) {
    text << " с помехой";
  }
  text << ": d20" << (attacker->attack_bonus >= 0 ? " + " : " - ")
       << std::abs(attacker->attack_bonus) << " = " << roll.breakdown;
  if (roll.breakdown != std::to_string(roll.total)) {
    text << " = " << roll.total;
  }
  text << " против КД " << target->ac;

  CommandOutcome outcome;
  outcome.accepted = true;
  int applied_damage = 0;
  const TokenId target_id = target->id;
  if (!check.hit) {
    text << " — промах";
  } else if (attacker->damage.empty()) {
    text << " — попадание, урон не задан";
  } else {
    const DiceRollResult damage_roll =
        RollAttackDamage(damage, check.critical, rng_);
    text << (check.critical ? " — критическое попадание, урон "
                            : " — попадание, урон ")
         << damage_roll.breakdown;
    if (damage_roll.breakdown != std::to_string(damage_roll.total)) {
      text << " = " << damage_roll.total;
    }
    applied_damage = std::max(0, damage_roll.total);
  }

  outcome.events.emplace_back(LogLineAdded{
      .line = LogLine{.author = AuthorName(actor),
                      .text = text.str(),
                      .timestamp = CurrentTimeSeconds()},
  });
  if (applied_damage > 0) {
    DamageAndLog(target_id, applied_damage, actor, outcome.events);
  }
  return outcome;
}

auto GameServer::HandleHeal(const HealToken& command, const std::string& actor,
                            bool can_control_all) -> CommandOutcome {
  const Token* token = FindToken(command.id);
  if (token == nullptr) {
    return CommandOutcome{.reject_reason = "токен не найден"};
  }
  if (!CanControl(*token, actor, can_control_all)) {
    return CommandOutcome{.reject_reason = ControlRejectReason(*token)};
  }
  if (command.amount <= 0 || command.amount > kMaxHp) {
    return CommandOutcome{.reject_reason = "некорректное лечение"};
  }
  const std::string name = token->name;
  const int hp_before = token->hp;
  const std::optional<Token> updated =
      state_.HealToken(command.id, command.amount);
  if (!updated.has_value()) {
    return CommandOutcome{.reject_reason = "не удалось вылечить"};
  }
  CommandOutcome outcome;
  outcome.accepted = true;
  outcome.events.emplace_back(TokenUpdated{.token = *updated});
  outcome.events.emplace_back(LogLineAdded{
      .line = LogLine{.author = AuthorName(actor),
                      .text = name + " восстановил " +
                              std::to_string(updated->hp - hp_before) + " HP",
                      .timestamp = CurrentTimeSeconds()},
  });
  return outcome;
}

auto GameServer::HandleRest(const RestParty& command, bool can_control_all)
    -> CommandOutcome {
  if (!can_control_all) {
    return CommandOutcome{.reject_reason = "отдых объявляет только DM"};
  }
  const bool has_players =
      std::ranges::any_of(state_.Tokens(), [](const Token& token) -> bool {
        return token.kind == TokenKind::kPlayer;
      });
  if (!has_players) {
    return CommandOutcome{.reject_reason = "в партии нет персонажей"};
  }
  const std::vector<RestChange> changed =
      state_.RestParty(command.long_rest, rng_);
  CommandOutcome outcome;
  outcome.accepted = true;
  for (const RestChange& change : changed) {
    outcome.events.emplace_back(TokenUpdated{.token = change.token});
  }
  std::string text;
  if (command.long_rest) {
    text =
        "Длинный отдых (8 часов): HP, кости хитов и состояния "
        "восстановлены";
  } else {
    std::vector<std::string> parts;
    for (const RestChange& change : changed) {
      if (change.dice_spent <= 0) {
        continue;
      }
      const int level = LevelForXp(change.token.xp);
      parts.emplace_back(
          change.token.name + " +" + std::to_string(change.hp_restored) +
          " HP (КХ " +
          std::to_string(HitDiceAvailable(level, change.token.hit_dice_spent)) +
          "/" + std::to_string(HitDiceMaximum(level)) + ")");
    }
    text = "Короткий отдых (1 час): ";
    text += parts.empty() ? "кости хитов не тратились" : JoinChanges(parts);
  }
  outcome.events.emplace_back(LogLineAdded{
      .line =
          LogLine{
              .author = "DM", .text = text, .timestamp = CurrentTimeSeconds()},
  });
  return outcome;
}

auto GameServer::HandleAdvanceTime(const AdvanceTime& command,
                                   bool can_control_all) -> CommandOutcome {
  if (!can_control_all) {
    return CommandOutcome{.reject_reason = "время двигает только DM"};
  }
  if (command.seconds <= 0 || command.seconds > kMaxConditionSeconds) {
    return CommandOutcome{.reject_reason = "некорректный интервал времени"};
  }
  if (state_.EncounterState().active) {
    return CommandOutcome{.reject_reason = "в бою время идёт ходами"};
  }
  const std::vector<Token> changed = state_.AdvanceTime(command.seconds);
  CommandOutcome outcome;
  outcome.accepted = true;
  for (const Token& token : changed) {
    outcome.events.emplace_back(TokenUpdated{.token = token});
  }
  outcome.events.emplace_back(LogLineAdded{
      .line = LogLine{.author = "DM",
                      .text = "Прошло времени: " +
                              FormatConditionDuration(command.seconds),
                      .timestamp = CurrentTimeSeconds()},
  });
  return outcome;
}

auto GameServer::HandleAoeDamage(const AoeDamage& command, bool can_control_all)
    -> CommandOutcome {
  if (!can_control_all) {
    return CommandOutcome{.reject_reason = "урон по области наносит только DM"};
  }
  const AoeTemplate& aoe = state_.Aoe();
  if (aoe.shape == AoeShape::kNone) {
    return CommandOutcome{.reject_reason = "сначала поставьте шаблон области"};
  }
  DiceExpression expression;
  if (command.expression.empty() ||
      command.expression.size() > kMaxDamageExpression ||
      !ParseDiceExpression(command.expression, expression)) {
    return CommandOutcome{.reject_reason = "некорректное выражение урона"};
  }
  if (command.save_dc < 1 || command.save_dc > 100) {
    return CommandOutcome{.reject_reason = "некорректная сложность спасброска"};
  }

  const DiceRollResult result = RollDiceExpression(expression, rng_);
  const int total_damage = std::max(0, result.total);
  const std::vector<Cell> cells = AoeCells(state_.Map(), aoe);
  CommandOutcome outcome;
  outcome.accepted = true;
  std::ostringstream header;
  header << "Область: " << command.expression << " = " << total_damage
         << ", спасбросок " << command.save_dc;
  outcome.events.emplace_back(LogLineAdded{
      .line = LogLine{.author = "DM",
                      .text = header.str(),
                      .timestamp = CurrentTimeSeconds()},
  });

  std::uniform_int_distribution<int> d20(1, 20);
  for (const Token& token : state_.Tokens()) {
    if (std::ranges::find(cells, token.cell) == cells.end()) {
      continue;
    }
    const std::string name = token.name;
    const int roll = d20(rng_) + token.save_bonus;
    const bool success = roll >= command.save_dc;
    const int damage = success ? total_damage / 2 : total_damage;
    if (damage > 0) {
      if (std::optional<Token> updated = state_.DamageToken(token.id, damage)) {
        outcome.events.emplace_back(TokenUpdated{.token = std::move(*updated)});
      }
    }
    std::ostringstream line;
    line << name << ": спасбросок " << roll << " против " << command.save_dc
         << (success ? " — успех, " : " — провал, ") << damage << " урона";
    outcome.events.emplace_back(LogLineAdded{
        .line = LogLine{.author = "DM",
                        .text = line.str(),
                        .timestamp = CurrentTimeSeconds()},
    });
  }
  return outcome;
}

auto GameServer::HandleDeathSave(const DeathSave& command,
                                 const std::string& actor, bool can_control_all)
    -> CommandOutcome {
  const Token* token = FindToken(command.id);
  if (token == nullptr) {
    return CommandOutcome{.reject_reason = "токен не найден"};
  }
  if (!CanControl(*token, actor, can_control_all)) {
    return CommandOutcome{.reject_reason = ControlRejectReason(*token)};
  }
  if (token->hp > 0) {
    return CommandOutcome{.reject_reason = "спасбросок нужен при 0 HP"};
  }
  const bool dead = std::ranges::any_of(token->conditions,
                                        [](const Condition& condition) -> bool {
                                          return condition.name == kDead;
                                        });
  if (dead) {
    return CommandOutcome{.reject_reason = "персонаж уже мёртв"};
  }
  const std::string name = token->name;
  std::uniform_int_distribution<int> d20(1, 20);
  const int roll = d20(rng_);
  const std::optional<Token> updated = state_.DeathSave(command.id, roll);
  if (!updated.has_value()) {
    return CommandOutcome{.reject_reason = "не удалось бросить спасбросок"};
  }
  std::ostringstream text;
  text << name << ": спасбросок от смерти " << roll;
  if (roll == 20) {
    text << " — критический успех, 1 HP";
  } else if (roll >= 10) {
    text << " — успех (" << updated->death_save_successes << "/3)";
    if (updated->death_save_successes >= 3) {
      text << ", стабилен";
    }
  } else {
    text << " — провал (" << updated->death_save_failures << "/3)";
    if (updated->death_save_failures >= 3) {
      text << ", персонаж погиб";
    }
  }
  CommandOutcome outcome;
  outcome.accepted = true;
  outcome.events.emplace_back(TokenUpdated{.token = *updated});
  outcome.events.emplace_back(LogLineAdded{
      .line = LogLine{.author = AuthorName(actor),
                      .text = text.str(),
                      .timestamp = CurrentTimeSeconds()},
  });
  return outcome;
}

auto GameServer::HandleSetInitiative(const SetInitiative& command,
                                     const std::string& actor,
                                     bool can_control_all) -> CommandOutcome {
  const Token* token = FindToken(command.id);
  if (token == nullptr) {
    return CommandOutcome{.reject_reason = "токен не найден"};
  }
  if (!CanControl(*token, actor, can_control_all)) {
    return CommandOutcome{.reject_reason = ControlRejectReason(*token)};
  }
  const bool encounter_active = state_.EncounterState().active;
  const std::optional<Token> updated =
      state_.SetInitiative(command.id, command.value);
  if (!updated.has_value()) {
    return CommandOutcome{.reject_reason = "не удалось обновить токен"};
  }
  CommandOutcome outcome;
  outcome.accepted = true;
  outcome.events.emplace_back(TokenUpdated{.token = *updated});
  if (encounter_active) {
    outcome.events.emplace_back(
        EncounterUpdated{.encounter = state_.EncounterState()});
  }
  return outcome;
}

auto GameServer::HandleClaimToken(const ClaimToken& command,
                                  const std::string& actor) -> CommandOutcome {
  const Token* token = FindToken(command.id);
  if (token == nullptr) {
    return CommandOutcome{.reject_reason = "токен не найден"};
  }
  if (token->kind != TokenKind::kPlayer) {
    return CommandOutcome{.reject_reason = "забрать можно только персонажа"};
  }
  if (actor.empty()) {
    return CommandOutcome{.reject_reason = "владельца назначает DM"};
  }
  if (!token->owner.empty() && !SamePlayerName(token->owner, actor)) {
    return CommandOutcome{.reject_reason = "токен контролирует другой игрок"};
  }
  for (const Token& other : state_.Tokens()) {
    if (other.id != command.id && other.kind == TokenKind::kPlayer &&
        SamePlayerName(other.owner, actor)) {
      return CommandOutcome{.reject_reason = "у игрока уже есть персонаж"};
    }
  }
  const std::optional<Token> updated = state_.SetTokenOwner(command.id, actor);
  if (!updated.has_value()) {
    return CommandOutcome{.reject_reason = "не удалось забрать персонажа"};
  }
  RecomputeFog();
  CommandOutcome outcome;
  outcome.accepted = true;
  outcome.events.emplace_back(TokenUpdated{.token = *updated});
  return outcome;
}

auto GameServer::HandleReleaseToken(const ReleaseToken& command,
                                    const std::string& actor)
    -> CommandOutcome {
  if (actor.empty()) {
    return CommandOutcome{.reject_reason = "DM использует /owner"};
  }
  const Token* token = FindToken(command.id);
  if (token == nullptr) {
    return CommandOutcome{.reject_reason = "токен не найден"};
  }
  if (!SamePlayerName(token->owner, actor)) {
    return CommandOutcome{.reject_reason = "это не ваш персонаж"};
  }
  const std::optional<Token> updated = state_.SetTokenOwner(command.id, "");
  if (!updated.has_value()) {
    return CommandOutcome{.reject_reason = "не удалось освободить персонажа"};
  }
  RecomputeFog();
  CommandOutcome outcome;
  outcome.accepted = true;
  outcome.events.emplace_back(TokenUpdated{.token = *updated});
  return outcome;
}

auto GameServer::HandleSetTokenOwner(const SetTokenOwner& command,
                                     bool can_control_all) -> CommandOutcome {
  if (!can_control_all) {
    return CommandOutcome{.reject_reason = "владельца назначает только DM"};
  }
  if (command.owner.size() > kMaxTokenName) {
    return CommandOutcome{.reject_reason = "имя владельца слишком длинное"};
  }
  const std::optional<Token> updated =
      state_.SetTokenOwner(command.id, command.owner);
  if (!updated.has_value()) {
    return CommandOutcome{.reject_reason = "токен не найден"};
  }
  RecomputeFog();
  CommandOutcome outcome;
  outcome.accepted = true;
  outcome.events.emplace_back(TokenUpdated{.token = *updated});
  return outcome;
}

auto GameServer::HandleRenameToken(const RenameToken& command,
                                   const std::string& actor,
                                   bool can_control_all) -> CommandOutcome {
  const Token* token = FindToken(command.id);
  if (token == nullptr) {
    return CommandOutcome{.reject_reason = "токен не найден"};
  }
  if (!CanControl(*token, actor, can_control_all)) {
    return CommandOutcome{.reject_reason = ControlRejectReason(*token)};
  }
  if (command.name.empty()) {
    return CommandOutcome{.reject_reason = "имя токена не может быть пустым"};
  }
  if (command.name.size() > kMaxTokenName) {
    return CommandOutcome{.reject_reason = "имя токена слишком длинное"};
  }
  const std::optional<Token> updated =
      state_.RenameToken(command.id, command.name);
  if (!updated.has_value()) {
    return CommandOutcome{.reject_reason = "не удалось переименовать токен"};
  }
  CommandOutcome outcome;
  outcome.accepted = true;
  outcome.events.emplace_back(TokenUpdated{.token = *updated});
  return outcome;
}

auto GameServer::HandleSetTokenMaxHp(const SetTokenMaxHp& command,
                                     const std::string& actor,
                                     bool can_control_all) -> CommandOutcome {
  const Token* token = FindToken(command.id);
  if (token == nullptr) {
    return CommandOutcome{.reject_reason = "токен не найден"};
  }
  if (!CanControl(*token, actor, can_control_all)) {
    return CommandOutcome{.reject_reason = ControlRejectReason(*token)};
  }
  if (command.max_hp <= 0 || command.max_hp > kMaxHp) {
    return CommandOutcome{.reject_reason = "некорректный максимум HP"};
  }
  const std::optional<Token> updated =
      state_.SetTokenMaxHp(command.id, command.max_hp);
  if (!updated.has_value()) {
    return CommandOutcome{.reject_reason = "не удалось обновить токен"};
  }
  CommandOutcome outcome;
  outcome.accepted = true;
  outcome.events.emplace_back(TokenUpdated{.token = *updated});
  return outcome;
}

auto GameServer::HandleSetTokenColor(const SetTokenColor& command,
                                     const std::string& actor,
                                     bool can_control_all) -> CommandOutcome {
  const Token* token = FindToken(command.id);
  if (token == nullptr) {
    return CommandOutcome{.reject_reason = "токен не найден"};
  }
  if (!CanControl(*token, actor, can_control_all)) {
    return CommandOutcome{.reject_reason = ControlRejectReason(*token)};
  }
  const std::optional<Token> updated =
      state_.SetTokenColor(command.id, command.color);
  if (!updated.has_value()) {
    return CommandOutcome{.reject_reason = "не удалось обновить токен"};
  }
  CommandOutcome outcome;
  outcome.accepted = true;
  outcome.events.emplace_back(TokenUpdated{.token = *updated});
  return outcome;
}

auto GameServer::HandleSetTokenPortrait(const SetTokenPortrait& command,
                                        const std::string& actor,
                                        bool can_control_all)
    -> CommandOutcome {
  const Token* token = FindToken(command.id);
  if (token == nullptr) {
    return CommandOutcome{.reject_reason = "токен не найден"};
  }
  if (!CanControl(*token, actor, can_control_all)) {
    return CommandOutcome{.reject_reason = ControlRejectReason(*token)};
  }
  const std::optional<Token> updated =
      state_.SetTokenPortrait(command.id, command.portrait);
  if (!updated.has_value()) {
    return CommandOutcome{.reject_reason = "некорректный портрет"};
  }
  CommandOutcome outcome;
  outcome.accepted = true;
  outcome.events.emplace_back(TokenUpdated{.token = *updated});
  return outcome;
}

auto GameServer::HandleUpdateTokenSheet(const UpdateTokenSheet& command,
                                        bool can_control_all)
    -> CommandOutcome {
  if (!can_control_all) {
    return CommandOutcome{.reject_reason = "лист редактирует только DM"};
  }
  const Token* token = FindToken(command.id);
  if (token == nullptr) {
    return CommandOutcome{.reject_reason = "токен не найден"};
  }
  if (command.ac <= 0 || command.ac > kMaxAc || command.speed < 0 ||
      command.speed > kMaxSpeed) {
    return CommandOutcome{.reject_reason = "некорректные КД или скорость"};
  }
  if (command.notes.size() > kMaxNotes) {
    return CommandOutcome{.reject_reason = "заметки слишком длинные"};
  }
  if (command.attack_bonus < -kMaxBonus || command.attack_bonus > kMaxBonus ||
      command.save_bonus < -kMaxBonus || command.save_bonus > kMaxBonus ||
      command.initiative_bonus < -kMaxBonus ||
      command.initiative_bonus > kMaxBonus) {
    return CommandOutcome{.reject_reason = "некорректный бонус"};
  }
  if (command.damage.size() > kMaxDamageExpression) {
    return CommandOutcome{.reject_reason = "слишком длинное выражение урона"};
  }
  if (!command.damage.empty()) {
    DiceExpression damage;
    if (!ParseDiceExpression(command.damage, damage)) {
      return CommandOutcome{.reject_reason = "некорректное выражение урона"};
    }
  }
  if (command.temp_hp < 0 || command.temp_hp > kMaxTempHp) {
    return CommandOutcome{.reject_reason = "некорректные временные HP"};
  }
  if (!IsValidHitDie(command.hit_die)) {
    return CommandOutcome{.reject_reason = "некорректная кость хитов"};
  }
  if (command.con_modifier < -kMaxConModifier ||
      command.con_modifier > kMaxConModifier) {
    return CommandOutcome{.reject_reason =
                              "некорректный модификатор Телосложения"};
  }
  const Token before = *token;
  const std::optional<Token> updated = state_.UpdateTokenSheet(
      command.id, command.ac, command.speed, command.notes,
      command.attack_bonus, command.save_bonus, command.initiative_bonus,
      command.damage, command.temp_hp, command.hit_die, command.con_modifier);
  if (!updated.has_value()) {
    return CommandOutcome{.reject_reason = "не удалось обновить лист"};
  }
  CommandOutcome outcome;
  outcome.accepted = true;
  outcome.events.emplace_back(TokenUpdated{.token = *updated});
  std::vector<std::string> changes;
  AppendIntChange(changes, "КД", before.ac, command.ac);
  AppendIntChange(changes, "скорость", before.speed, command.speed);
  AppendIntChange(changes, "бонус атаки", before.attack_bonus,
                  command.attack_bonus);
  AppendIntChange(changes, "бонус спасброска", before.save_bonus,
                  command.save_bonus);
  AppendIntChange(changes, "бонус инициативы", before.initiative_bonus,
                  command.initiative_bonus);
  AppendIntChange(changes, "временные HP", before.temp_hp, command.temp_hp);
  AppendIntChange(changes, "мод. Телосложения", before.con_modifier,
                  command.con_modifier);
  if (before.hit_die != command.hit_die) {
    changes.emplace_back("кость хитов d" + std::to_string(command.hit_die));
  }
  if (before.damage != command.damage) {
    changes.emplace_back("урон " +
                         (command.damage.empty() ? "нет" : command.damage));
  }
  if (before.notes != command.notes) {
    changes.emplace_back("заметки");
  }
  if (!changes.empty()) {
    outcome.events.emplace_back(LogLineAdded{
        .line =
            LogLine{.author = "DM",
                    .text = "Лист " + before.name + ": " + JoinChanges(changes),
                    .timestamp = CurrentTimeSeconds()},
    });
  }
  return outcome;
}

auto GameServer::HandleUpdateTokenCombat(const UpdateTokenCombat& command,
                                         bool can_control_all)
    -> CommandOutcome {
  if (!can_control_all) {
    return CommandOutcome{.reject_reason = "состояния меняет только DM"};
  }
  const Token* token = FindToken(command.id);
  if (token == nullptr) {
    return CommandOutcome{.reject_reason = "токен не найден"};
  }
  if (command.conditions.size() > kMaxConditions) {
    return CommandOutcome{.reject_reason = "слишком много состояний"};
  }
  for (const Condition& condition : command.conditions) {
    if (condition.name.empty() || condition.name.size() > kMaxConditionLength ||
        condition.seconds < 0 || condition.seconds > kMaxConditionSeconds ||
        condition.save_dc < 0 || condition.save_dc > kMaxSaveDc) {
      return CommandOutcome{.reject_reason = "некорректное состояние"};
    }
  }
  const Token before = *token;
  const std::optional<Token> updated =
      state_.UpdateTokenCombat(command.id, command.conditions);
  if (!updated.has_value()) {
    return CommandOutcome{.reject_reason = "не удалось обновить состояния"};
  }
  CommandOutcome outcome;
  outcome.accepted = true;
  outcome.events.emplace_back(TokenUpdated{.token = *updated});
  std::vector<std::string> changes;
  const auto has_condition = [](const std::vector<Condition>& list,
                                const std::string& name) -> bool {
    return std::ranges::any_of(list, [&name](const Condition& item) -> bool {
      return item.name == name;
    });
  };
  for (const Condition& condition : command.conditions) {
    const auto before_condition = std::ranges::find_if(
        before.conditions, [&condition](const Condition& item) -> bool {
          return item.name == condition.name;
        });
    if (before_condition == before.conditions.end()) {
      std::string entry = "+" + condition.name;
      entry += " (" + FormatConditionDuration(condition.seconds);
      if (condition.save_dc > 0) {
        entry += ", КС " + std::to_string(condition.save_dc);
      }
      entry += ")";
      changes.push_back(std::move(entry));
      continue;
    }
    if (before_condition->save_dc != condition.save_dc) {
      changes.push_back(condition.name + ": КС " +
                        std::to_string(before_condition->save_dc) + "→" +
                        std::to_string(condition.save_dc));
    }
  }
  for (const Condition& condition : before.conditions) {
    if (!has_condition(command.conditions, condition.name)) {
      changes.emplace_back("−" + condition.name);
    }
  }
  if (!changes.empty()) {
    outcome.events.emplace_back(LogLineAdded{
        .line = LogLine{.author = "DM",
                        .text = "Состояния " + before.name + ": " +
                                JoinChanges(changes),
                        .timestamp = CurrentTimeSeconds()},
    });
  }
  return outcome;
}

auto GameServer::HandleSetAoe(const SetAoe& command, bool can_control_all)
    -> CommandOutcome {
  if (!can_control_all) {
    return CommandOutcome{.reject_reason = "шаблон области ставит только DM"};
  }
  if (!state_.SetAoe(command.aoe)) {
    return CommandOutcome{.reject_reason = "некорректный шаблон области"};
  }
  CommandOutcome outcome;
  outcome.accepted = true;
  outcome.events.emplace_back(AoeUpdated{.aoe = state_.Aoe()});
  return outcome;
}

auto GameServer::HandlePing(const PingAt& command, const std::string& actor)
    -> CommandOutcome {
  if (!state_.Map().Contains(command.cell)) {
    return CommandOutcome{.reject_reason = "Метка за пределами карты"};
  }
  CommandOutcome outcome;
  outcome.accepted = true;
  outcome.events.emplace_back(
      PingMarker{.cell = command.cell, .author = AuthorName(actor)});
  return outcome;
}

auto GameServer::HandleSetMapBackground(const SetMapBackground& command,
                                        bool can_control_all)
    -> CommandOutcome {
  if (!can_control_all) {
    return CommandOutcome{.reject_reason = "подложку карты меняет только DM"};
  }
  if (!state_.SetBackground(command.background)) {
    return CommandOutcome{.reject_reason = "некорректная подложка карты"};
  }
  CommandOutcome outcome;
  outcome.accepted = true;
  outcome.events.emplace_back(
      MapBackgroundUpdated{.background = state_.Background()});
  return outcome;
}

auto GameServer::HandleStartEncounter(bool can_control_all) -> CommandOutcome {
  if (!can_control_all) {
    return CommandOutcome{.reject_reason = "бой начинает только DM"};
  }
  const std::vector<TokenId> spent = TokensWithSpentMovement(state_);
  // Инициатива бросается за всех сразу: d20 + бонус из листа. Это же
  // значение потом можно поправить вручную через [ и ].
  std::uniform_int_distribution<int> d20(1, 20);
  std::vector<std::pair<int, std::string>> rolls;
  rolls.reserve(state_.Tokens().size());
  for (const Token& token : state_.Tokens()) {
    const int value = d20(rng_) + token.initiative_bonus;
    rolls.emplace_back(value, token.name);
    (void)state_.SetInitiative(token.id, value);
  }
  if (!state_.StartEncounter()) {
    return CommandOutcome{.reject_reason = "нет токенов для инициативы"};
  }
  std::ranges::sort(rolls, [](const auto& left, const auto& right) -> bool {
    return left.first > right.first;
  });
  std::ostringstream initiative;
  initiative << "Инициатива:";
  for (const auto& [value, name] : rolls) {
    initiative << " " << name << " " << value << ";";
  }
  CommandOutcome outcome;
  outcome.accepted = true;
  outcome.events.emplace_back(
      EncounterUpdated{.encounter = state_.EncounterState()});
  outcome.events.emplace_back(LogLineAdded{
      .line = LogLine{.author = "DM",
                      .text = initiative.str(),
                      .timestamp = CurrentTimeSeconds()},
  });
  for (const TokenId id : spent) {
    if (std::optional<Token> token = state_.TokenCopy(id)) {
      outcome.events.emplace_back(TokenUpdated{.token = std::move(*token)});
    }
  }
  return outcome;
}

auto GameServer::HandleEndEncounter(bool can_control_all) -> CommandOutcome {
  if (!can_control_all) {
    return CommandOutcome{.reject_reason = "бой завершает только DM"};
  }
  if (!state_.EncounterState().active) {
    return CommandOutcome{.reject_reason = "нет активного боя"};
  }
  const std::vector<TokenId> spent = TokensWithSpentMovement(state_);
  state_.EndEncounter();
  CommandOutcome outcome;
  outcome.accepted = true;
  outcome.events.emplace_back(
      EncounterUpdated{.encounter = state_.EncounterState()});
  for (const TokenId id : spent) {
    if (std::optional<Token> token = state_.TokenCopy(id)) {
      outcome.events.emplace_back(TokenUpdated{.token = std::move(*token)});
    }
  }
  return outcome;
}

auto GameServer::HandleNextTurn(bool can_control_all) -> CommandOutcome {
  if (!can_control_all) {
    return CommandOutcome{.reject_reason = "ход переключает только DM"};
  }
  if (!state_.EncounterState().active) {
    return CommandOutcome{.reject_reason = "нет активного боя"};
  }
  CommandOutcome outcome;
  outcome.accepted = true;
  std::vector<TokenId> changed;
  const auto remember = [&changed](TokenId id) -> void {
    if (std::ranges::find(changed, id) == changed.end()) {
      changed.push_back(id);
    }
  };

  // Конец хода активного токена: состояния с КС дают спасбросок d20 +
  // бонус спасброска. Успех снимает состояние, провал оставляет его.
  const TokenId ending_id = state_.ActiveTokenId();
  if (const std::optional<Token> ending = state_.TokenCopy(ending_id)) {
    std::uniform_int_distribution<int> d20(1, 20);
    for (const Condition& condition : ending->conditions) {
      if (condition.save_dc <= 0) {
        continue;
      }
      const int roll = d20(rng_) + ending->save_bonus;
      const bool success = roll >= condition.save_dc;
      std::string text = "спасбросок " + std::to_string(roll) + " против КС " +
                         std::to_string(condition.save_dc);
      if (success) {
        text += " — снято «" + condition.name + "»";
        if (state_.RemoveTokenCondition(ending_id, condition.name)
                .has_value()) {
          remember(ending_id);
        }
      } else {
        text += " — остаётся «" + condition.name + "»";
      }
      outcome.events.emplace_back(LogLineAdded{
          .line = LogLine{.author = ending->name,
                          .text = std::move(text),
                          .timestamp = CurrentTimeSeconds()},
      });
    }
  }

  const std::vector<TokenId> spent = TokensWithSpentMovement(state_);
  if (!state_.NextTurn()) {
    return CommandOutcome{.reject_reason = "нет активного боя"};
  }
  for (const TokenId id : spent) {
    remember(id);
  }
  // У нового активного токена время хода уже срезало его состояния.
  remember(state_.ActiveTokenId());
  for (const TokenId id : changed) {
    if (std::optional<Token> token = state_.TokenCopy(id)) {
      outcome.events.emplace_back(TokenUpdated{.token = std::move(*token)});
    }
  }
  outcome.events.emplace_back(
      EncounterUpdated{.encounter = state_.EncounterState()});
  return outcome;
}

auto GameServer::HandleReveal(const RevealArea& command, bool can_control_all)
    -> CommandOutcome {
  if (!can_control_all) {
    return CommandOutcome{.reject_reason = "области открывает только DM"};
  }
  if (command.radius <= 0 || command.radius > kMaxRevealRadiusCells) {
    return CommandOutcome{.reject_reason = "некорректный радиус открытия"};
  }
  if (!state_.Map().Contains(command.cell)) {
    return CommandOutcome{.reject_reason = "клетка за пределами карты"};
  }
  if (!state_.RevealArea(command.cell, command.radius)) {
    return CommandOutcome{.accepted = true};
  }
  RecomputeFog();
  CommandOutcome outcome;
  outcome.accepted = true;
  outcome.events.emplace_back(RevealedUpdated{.cells = state_.Revealed()});
  return outcome;
}

auto GameServer::HandleHide(const HideArea& command, bool can_control_all)
    -> CommandOutcome {
  if (!can_control_all) {
    return CommandOutcome{.reject_reason = "области закрывает только DM"};
  }
  if (command.radius <= 0 || command.radius > kMaxRevealRadiusCells) {
    return CommandOutcome{.reject_reason = "некорректный радиус закрытия"};
  }
  if (!state_.Map().Contains(command.cell)) {
    return CommandOutcome{.reject_reason = "клетка за пределами карты"};
  }
  if (!state_.HideArea(command.cell, command.radius)) {
    return CommandOutcome{.accepted = true};
  }
  RecomputeFog();
  CommandOutcome outcome;
  outcome.accepted = true;
  outcome.events.emplace_back(RevealedUpdated{.cells = state_.Revealed()});
  return outcome;
}

auto GameServer::HandleClearRevealed(bool can_control_all) -> CommandOutcome {
  if (!can_control_all) {
    return CommandOutcome{.reject_reason = "области открывает только DM"};
  }
  if (!state_.ClearRevealed()) {
    return CommandOutcome{.accepted = true};
  }
  RecomputeFog();
  CommandOutcome outcome;
  outcome.accepted = true;
  outcome.events.emplace_back(RevealedUpdated{});
  return outcome;
}

auto GameServer::HandleAwardXp(const AwardXp& command, bool can_control_all)
    -> CommandOutcome {
  if (!can_control_all) {
    return CommandOutcome{.reject_reason = "опыт начисляет только DM"};
  }
  if (command.amount <= 0 || command.amount > kMaxXpAward) {
    return CommandOutcome{.reject_reason = "некорректное количество опыта"};
  }

  std::vector<TokenId> targets;
  if (command.party) {
    for (const Token& token : state_.Tokens()) {
      if (token.kind == TokenKind::kPlayer) {
        targets.push_back(token.id);
      }
    }
    if (targets.empty()) {
      return CommandOutcome{.reject_reason = "в партии нет персонажей"};
    }
  } else {
    const Token* token = FindToken(command.id);
    if (token == nullptr) {
      return CommandOutcome{.reject_reason = "токен не найден"};
    }
    if (token->kind != TokenKind::kPlayer) {
      return CommandOutcome{.reject_reason = "опыт получают только персонажи"};
    }
    targets.push_back(command.id);
  }

  CommandOutcome outcome;
  outcome.accepted = true;
  std::ostringstream summary;
  if (command.party) {
    summary << "Партия получает " << command.amount << " опыта";
  } else {
    summary << FindToken(targets.front())->name << " получает "
            << command.amount << " опыта";
  }
  outcome.events.emplace_back(LogLineAdded{
      .line = LogLine{.author = "DM",
                      .text = summary.str(),
                      .timestamp = CurrentTimeSeconds()},
  });

  for (const TokenId id : targets) {
    const Token* token = FindToken(id);
    const std::string name = token->name;
    const int before = LevelForXp(token->xp);
    const int hp_per_level =
        HitPointsPerLevel(token->hit_die, token->con_modifier);
    const std::optional<Token> updated = state_.AddXp(id, command.amount);
    if (!updated.has_value()) {
      continue;
    }
    outcome.events.emplace_back(TokenUpdated{.token = *updated});
    const int after = LevelForXp(updated->xp);
    for (int level = before + 1; level <= after; ++level) {
      std::ostringstream line;
      line << name << " получает " << level << " уровень: +" << hp_per_level
           << " к максимуму HP";
      if (ProficiencyBonus(level) > ProficiencyBonus(level - 1)) {
        line << ", бонус мастерства +" << ProficiencyBonus(level);
      }
      if (IsAbilityScoreLevel(level)) {
        line << ", повышение характеристик";
      } else if (level == kEpicBoonLevel) {
        line << ", эпический дар";
      }
      outcome.events.emplace_back(LogLineAdded{
          .line = LogLine{.author = "DM",
                          .text = line.str(),
                          .timestamp = CurrentTimeSeconds()},
      });
    }
  }
  return outcome;
}

auto GameServer::PlayerFog(std::string_view actor) const -> const FogOfWar* {
  const auto it = fogs_.find(PlayerNameKey(actor));
  return it == fogs_.end() ? nullptr : &it->second;
}

auto GameServer::IsVisibleTo(std::string_view actor, Cell cell) const -> bool {
  if (const FogOfWar* fog = PlayerFog(actor); fog != nullptr) {
    if (fog->IsVisible(cell)) {
      return true;
    }
  } else if (party_fog_.IsVisible(cell)) {
    // Игрок ещё не выбрал персонажа: ему показывают общий вид партии.
    return true;
  }
  return state_.IsRevealed(cell);
}

auto GameServer::IsExploredTo(std::string_view actor, Cell cell) const -> bool {
  const FogOfWar* own = PlayerFog(actor);
  const FogOfWar& fog = own != nullptr ? *own : party_fog_;
  return fog.IsExplored(cell) || state_.IsRevealed(cell);
}

auto GameServer::Snapshot() const -> GameSnapshot {
  GameSnapshot snapshot = state_.Snapshot();
  snapshot.fog = FogRecords();
  return snapshot;
}

auto GameServer::ClientSnapshot(std::string_view actor) const -> GameSnapshot {
  GameSnapshot snapshot = state_.Snapshot();
  std::erase_if(snapshot.tokens, [this, actor](const Token& token) -> bool {
    return !IsVisibleTo(actor, token.cell);
  });
  // Открытые DM-ом клетки клиенту не нужны: их видимость уже в тумане, а
  // список может быть длинным и повторялся бы в каждом обновлении вида.
  snapshot.revealed.clear();
  // Туман других игроков по сети не ходит: клиент получает только свой.
  snapshot.fog.clear();
  return snapshot;
}

auto GameServer::FogState(std::string_view actor) const -> FogSnapshot {
  if (const FogOfWar* fog = PlayerFog(actor); fog != nullptr) {
    return fog->Snapshot();
  }
  // До выбора персонажа игрок видит общий обзор партии; после — только свой.
  return party_fog_.Snapshot();
}

auto GameServer::FullFogState() const -> FogSnapshot {
  const int width = state_.Map().Width();
  const int height = state_.Map().Height();
  const auto cells =
      static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
  return FogSnapshot{
      .width = width,
      .height = height,
      .visible = std::vector<std::uint8_t>(cells, 1),
      .explored = std::vector<std::uint8_t>(cells, 1),
  };
}

auto GameServer::EventVisibleTo(const Event& event,
                                std::string_view actor) const -> bool {
  return std::visit(
      [this, actor](const auto& ev) -> bool {
        using T = std::decay_t<decltype(ev)>;
        if constexpr (std::is_same_v<T, TokenMoved>) {
          return IsVisibleTo(actor, ev.to);
        } else if constexpr (std::is_same_v<T, TokenAdded> ||
                             std::is_same_v<T, TokenUpdated>) {
          return IsVisibleTo(actor, ev.token.cell);
        }
        return true;
      },
      event);
}

auto GameServer::TakeViewDelta(std::string_view actor) -> std::vector<Event> {
  std::vector<Event> events;

  std::vector<TokenId> visible;
  visible.reserve(state_.Tokens().size());
  for (const Token& token : state_.Tokens()) {
    if (IsVisibleTo(actor, token.cell)) {
      visible.push_back(token.id);
    }
  }
  std::ranges::sort(visible);

  // Слепка может не быть (пир только подключился и ещё не получал полный
  // вид): тогда дельта — это весь видимый мир, а не потеря токенов.
  SentView& sent = sent_views_[PlayerNameKey(actor)];
  std::vector<TokenId> appeared;
  std::ranges::set_difference(visible, sent.tokens,
                              std::back_inserter(appeared));
  for (const TokenId id : appeared) {
    if (const Token* token = FindToken(id); token != nullptr) {
      events.emplace_back(TokenAdded{.token = *token});
    }
  }
  std::vector<TokenId> vanished;
  std::ranges::set_difference(sent.tokens, visible,
                              std::back_inserter(vanished));
  for (const TokenId id : vanished) {
    events.emplace_back(TokenRemoved{.id = id});
  }
  sent.tokens = std::move(visible);

  FogSnapshot fog = FogState(actor);
  const std::size_t cells = fog.visible.size();
  const bool same_map =
      sent.fog.width == fog.width && sent.fog.height == fog.height &&
      sent.fog.visible.size() == cells && sent.fog.explored.size() == cells &&
      fog.explored.size() == cells;
  const int width = std::max(fog.width, 1);
  const auto fog_cell = [&fog, width](std::size_t index) -> FogCell {
    return FogCell{
        .cell =
            Cell{
                .x = static_cast<int>(index % static_cast<std::size_t>(width)),
                .y = static_cast<int>(index / static_cast<std::size_t>(width))},
        .visible = fog.visible[index] != 0,
        .explored = fog.explored[index] != 0,
    };
  };
  FogChanged changed;
  for (std::size_t i = 0; i < cells; ++i) {
    // Другая карта: точечная правка невозможна, отдаём туман целиком.
    if (same_map && sent.fog.visible[i] == fog.visible[i] &&
        sent.fog.explored[i] == fog.explored[i]) {
      continue;
    }
    changed.cells.push_back(fog_cell(i));
  }
  if (!changed.cells.empty()) {
    events.emplace_back(std::move(changed));
  }
  sent.fog = std::move(fog);
  return events;
}

auto GameServer::MarkViewSent(std::string_view actor) -> void {
  SentView view;
  view.fog = FogState(actor);
  view.tokens.reserve(state_.Tokens().size());
  for (const Token& token : state_.Tokens()) {
    if (IsVisibleTo(actor, token.cell)) {
      view.tokens.push_back(token.id);
    }
  }
  std::ranges::sort(view.tokens);
  sent_views_.insert_or_assign(PlayerNameKey(actor), std::move(view));
}

auto GameServer::FindToken(TokenId id) const -> const Token* {
  for (const Token& token : state_.Tokens()) {
    if (token.id == id) {
      return &token;
    }
  }
  return nullptr;
}

auto GameServer::RestoreFog(const std::vector<FogRecord>& records) -> void {
  const int width = state_.Map().Width();
  const int height = state_.Map().Height();
  const auto cells =
      static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
  fogs_.clear();
  party_fog_ = FogOfWar(width, height);
  for (const FogRecord& record : records) {
    // Снимок другой карты: исследованное некуда переносить, начинаем заново.
    if (record.explored.size() != cells) {
      continue;
    }
    FogOfWar fog(width, height);
    fog.Restore(FogSnapshot{
        .width = width,
        .height = height,
        .explored = record.explored,
    });
    if (record.owner.empty()) {
      party_fog_ = std::move(fog);
    } else {
      fogs_.insert_or_assign(PlayerNameKey(record.owner), std::move(fog));
    }
  }
}

auto GameServer::FogRecords() const -> std::vector<FogRecord> {
  std::vector<FogRecord> records;
  records.reserve(fogs_.size() + 1);
  records.push_back(FogRecord{.owner = {}, .explored = party_fog_.Explored()});
  for (const auto& [owner, fog] : fogs_) {
    records.push_back(FogRecord{.owner = owner, .explored = fog.Explored()});
  }
  return records;
}

auto GameServer::RecomputeFog() -> void {
  // Обзор дают только персонажи; у каждого игрока свои наблюдатели.
  std::map<std::string, std::vector<Cell>, std::less<>> observers;
  std::vector<Cell> party_observers;
  for (const Token& token : state_.Tokens()) {
    if (token.kind != TokenKind::kPlayer) {
      continue;
    }
    party_observers.push_back(token.cell);
    if (!token.owner.empty()) {
      observers[PlayerNameKey(token.owner)].push_back(token.cell);
    }
  }

  const std::vector<Cell>& revealed = state_.Revealed();
  party_fog_.Recompute(state_.Map(), party_observers, sight_radius_);
  party_fog_.Reveal(revealed);
  // Игрок без персонажа больше не видит клетку, но помнит исследованное.
  for (auto& [owner, fog] : fogs_) {
    const auto it = observers.find(owner);
    fog.Recompute(state_.Map(),
                  it == observers.end() ? std::vector<Cell>{} : it->second,
                  sight_radius_);
    fog.Reveal(revealed);
  }
  for (const auto& [owner, cells] : observers) {
    if (fogs_.contains(owner)) {
      continue;
    }
    FogOfWar fog(state_.Map().Width(), state_.Map().Height());
    fog.Recompute(state_.Map(), cells, sight_radius_);
    fog.Reveal(revealed);
    fogs_.emplace(owner, std::move(fog));
  }
}

}  // namespace dnd
