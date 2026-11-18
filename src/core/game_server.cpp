#include "core/game_server.h"

#include <algorithm>
#include <optional>
#include <sstream>
#include <type_traits>
#include <utility>

#include "core/clock.h"
#include "core/dice.h"

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

}  // namespace

GameServer::GameServer(GameSnapshot snapshot, int sight_radius,
                       std::uint32_t rng_seed)
    : state_(std::move(snapshot)),
      fog_(state_.Map().Width(), state_.Map().Height()),
      sight_radius_(sight_radius),
      rng_(rng_seed) {
  RecomputeFog();
}

auto GameServer::Reset(GameSnapshot snapshot) -> void {
  state_ = GameState(std::move(snapshot));
  fog_ = FogOfWar(state_.Map().Width(), state_.Map().Height());
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
        } else if constexpr (std::is_same_v<T, UpdateTokenSheet>) {
          return HandleUpdateTokenSheet(cmd, actor, can_control_all);
        } else if constexpr (std::is_same_v<T, UpdateTokenCombat>) {
          return HandleUpdateTokenCombat(cmd, actor, can_control_all);
        } else if constexpr (std::is_same_v<T, StartEncounter>) {
          return HandleStartEncounter(can_control_all);
        } else if constexpr (std::is_same_v<T, EndEncounter>) {
          return HandleEndEncounter(can_control_all);
        } else if constexpr (std::is_same_v<T, SetAoe>) {
          return HandleSetAoe(cmd, can_control_all);
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
  return token.owner == actor;
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

  const std::optional<Event> event = state_.ApplyCommand(command);
  if (!event.has_value()) {
    return CommandOutcome{.reject_reason = "недопустимый ход"};
  }
  RecomputeFog();

  CommandOutcome outcome;
  outcome.accepted = true;
  outcome.events.emplace_back(*event);
  outcome.events.emplace_back(ViewUpdated{
      .snapshot = ClientSnapshot(),
      .fog = fog_.Snapshot(),
  });
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
    outcome.events.emplace_back(ViewUpdated{
        .snapshot = ClientSnapshot(),
        .fog = fog_.Snapshot(),
    });
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
    outcome.events.emplace_back(ViewUpdated{
        .snapshot = ClientSnapshot(),
        .fog = fog_.Snapshot(),
    });
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
  if (!token->owner.empty() && token->owner != actor) {
    return CommandOutcome{.reject_reason = "токен контролирует другой игрок"};
  }
  for (const Token& other : state_.Tokens()) {
    if (other.id != command.id && other.kind == TokenKind::kPlayer &&
        other.owner == actor) {
      return CommandOutcome{.reject_reason = "у игрока уже есть персонаж"};
    }
  }
  const std::optional<Token> updated = state_.SetTokenOwner(command.id, actor);
  if (!updated.has_value()) {
    return CommandOutcome{.reject_reason = "не удалось забрать персонажа"};
  }
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
  if (token->owner != actor) {
    return CommandOutcome{.reject_reason = "это не ваш персонаж"};
  }
  const std::optional<Token> updated = state_.SetTokenOwner(command.id, "");
  if (!updated.has_value()) {
    return CommandOutcome{.reject_reason = "не удалось освободить персонажа"};
  }
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

auto GameServer::HandleUpdateTokenSheet(const UpdateTokenSheet& command,
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
  if (command.temp_hp < 0 || command.temp_hp > kMaxTempHp) {
    return CommandOutcome{.reject_reason = "некорректные временные HP"};
  }
  const std::optional<Token> updated = state_.UpdateTokenSheet(
      command.id, command.ac, command.speed, command.notes,
      command.attack_bonus, command.save_bonus, command.damage,
      command.temp_hp);
  if (!updated.has_value()) {
    return CommandOutcome{.reject_reason = "не удалось обновить лист"};
  }
  CommandOutcome outcome;
  outcome.accepted = true;
  outcome.events.emplace_back(TokenUpdated{.token = *updated});
  return outcome;
}

auto GameServer::HandleUpdateTokenCombat(const UpdateTokenCombat& command,
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
  if (command.conditions.size() > kMaxConditions) {
    return CommandOutcome{.reject_reason = "слишком много состояний"};
  }
  for (const std::string& condition : command.conditions) {
    if (condition.empty() || condition.size() > kMaxConditionLength) {
      return CommandOutcome{.reject_reason = "некорректное состояние"};
    }
  }
  const std::optional<Token> updated =
      state_.UpdateTokenCombat(command.id, command.conditions);
  if (!updated.has_value()) {
    return CommandOutcome{.reject_reason = "не удалось обновить состояния"};
  }
  CommandOutcome outcome;
  outcome.accepted = true;
  outcome.events.emplace_back(TokenUpdated{.token = *updated});
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

auto GameServer::HandleStartEncounter(bool can_control_all) -> CommandOutcome {
  if (!can_control_all) {
    return CommandOutcome{.reject_reason = "бой начинает только DM"};
  }
  if (!state_.StartEncounter()) {
    return CommandOutcome{.reject_reason = "нет токенов для инициативы"};
  }
  CommandOutcome outcome;
  outcome.accepted = true;
  outcome.events.emplace_back(
      EncounterUpdated{.encounter = state_.EncounterState()});
  return outcome;
}

auto GameServer::HandleEndEncounter(bool can_control_all) -> CommandOutcome {
  if (!can_control_all) {
    return CommandOutcome{.reject_reason = "бой завершает только DM"};
  }
  if (!state_.EncounterState().active) {
    return CommandOutcome{.reject_reason = "нет активного боя"};
  }
  state_.EndEncounter();
  CommandOutcome outcome;
  outcome.accepted = true;
  outcome.events.emplace_back(
      EncounterUpdated{.encounter = state_.EncounterState()});
  return outcome;
}

auto GameServer::HandleNextTurn(bool can_control_all) -> CommandOutcome {
  if (!can_control_all) {
    return CommandOutcome{.reject_reason = "ход переключает только DM"};
  }
  if (!state_.NextTurn()) {
    return CommandOutcome{.reject_reason = "нет активного боя"};
  }
  CommandOutcome outcome;
  outcome.accepted = true;
  outcome.events.emplace_back(
      EncounterUpdated{.encounter = state_.EncounterState()});
  return outcome;
}

auto GameServer::ClientSnapshot() const -> GameSnapshot {
  GameSnapshot snapshot = state_.Snapshot();
  std::erase_if(snapshot.tokens, [this](const Token& token) -> bool {
    return !fog_.IsVisible(token.cell);
  });
  return snapshot;
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

auto GameServer::EventVisibleToPlayers(const Event& event) const -> bool {
  return std::visit(
      [this](const auto& ev) -> bool {
        using T = std::decay_t<decltype(ev)>;
        if constexpr (std::is_same_v<T, TokenMoved>) {
          return fog_.IsVisible(ev.to);
        } else if constexpr (std::is_same_v<T, TokenAdded> ||
                             std::is_same_v<T, TokenUpdated>) {
          return fog_.IsVisible(ev.token.cell);
        }
        return true;
      },
      event);
}

auto GameServer::FindToken(TokenId id) const -> const Token* {
  for (const Token& token : state_.Tokens()) {
    if (token.id == id) {
      return &token;
    }
  }
  return nullptr;
}

auto GameServer::RecomputeFog() -> void {
  std::vector<Cell> observer_cells;
  for (const Token& token : state_.Tokens()) {
    if (token.kind == TokenKind::kPlayer) {
      observer_cells.push_back(token.cell);
    }
  }
  fog_.Recompute(state_.Map(), observer_cells, sight_radius_);
}

}  // namespace dnd
