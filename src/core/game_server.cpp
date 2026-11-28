#include "core/game_server.h"

#include <algorithm>
#include <cstdlib>
#include <iterator>
#include <optional>
#include <ranges>
#include <sstream>
#include <string_view>
#include <type_traits>
#include <utility>

#include "core/attack.h"
#include "core/clock.h"
#include "core/conditions.h"
#include "core/dice.h"
#include "core/levels.h"
#include "core/movement.h"
#include "core/ownership.h"
#include "core/player_name.h"
#include "core/scene_light.h"
#include "core/skills.h"
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

// Русское склонение для числа костей хитов в строке отдыха.
auto HitDiceWord(int count) -> std::string_view {
  const int last_two = count % 100;
  const int last = count % 10;
  if (last_two >= 11 && last_two <= 14) {
    return "костей";
  }
  if (last == 1) {
    return "кость";
  }
  if (last >= 2 && last <= 4) {
    return "кости";
  }
  return "костей";
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

// Токены, чьё состояние хода нужно показать клиентам после сброса.
auto TokensWithTurnState(const GameState& state) -> std::vector<TokenId> {
  std::vector<TokenId> ids;
  for (const Token& token : state.Tokens()) {
    if (token.movement_used != 0 || token.action_used ||
        token.bonus_action_used || token.reaction_used ||
        token.movement_bonus != 0 || token.disengaged || token.dodging) {
      ids.push_back(token.id);
    }
  }
  return ids;
}

auto TurnActionName(TurnActionKind kind) -> const char* {
  switch (kind) {
    case TurnActionKind::kDash:
      return "Рывок";
    case TurnActionKind::kDisengage:
      return "Отход";
    case TurnActionKind::kDodge:
      return "Уклонение";
    case TurnActionKind::kHelp:
      return "Помощь";
    case TurnActionKind::kHide:
      return "Скрыться";
    case TurnActionKind::kSearch:
      return "Поиск";
    case TurnActionKind::kStudy:
      return "Изучение";
    case TurnActionKind::kUseObject:
      return "Использовать предмет";
    case TurnActionKind::kReady:
      return "Подготовиться";
  }
  return "Действие";
}

// Бросок d20 с модификатором в тексте лога: «d20 + 3 = 15».
auto FormatD20Roll(int bonus, const DiceRollResult& roll) -> std::string {
  std::ostringstream text;
  text << "d20";
  if (bonus > 0) {
    text << " + " << bonus;
  } else if (bonus < 0) {
    text << " - " << -bonus;
  }
  text << " = " << roll.breakdown;
  if (roll.breakdown != std::to_string(roll.total)) {
    text << " = " << roll.total;
  }
  return text.str();
}

// Исход проверки словом: «успех» или «провал».
auto CheckOutcomeWord(int total, int dc) -> std::string_view {
  return total >= dc ? "успех" : "провал";
}

auto TurnActionEffect(TurnActionKind kind, const Token& token) -> std::string {
  switch (kind) {
    case TurnActionKind::kDash:
      return " — ещё " + FormatMeters(EffectiveSpeedCells(token)) + " движения";
    case TurnActionKind::kDisengage:
      return " — движение не вызывает атак по возможности";
    case TurnActionKind::kDodge:
      return " — атаки по нему с помехой до начала его следующего хода";
    case TurnActionKind::kHelp:
      return " — преимущество союзнику (на столе)";
    case TurnActionKind::kHide:
      return " — скрывается";
    case TurnActionKind::kSearch:
      return " — осматривает местность";
    case TurnActionKind::kStudy:
      return " — вспоминает сведения";
    case TurnActionKind::kUseObject:
      return " — использует предмет";
    case TurnActionKind::kReady:
      return " — готовит отложенное действие";
  }
  return {};
}

// Атака по возможности бьёт только цель в 5 футах, то есть в соседней клетке.
auto AreAdjacent(Cell left, Cell right) -> bool {
  return std::abs(left.x - right.x) <= 1 && std::abs(left.y - right.y) <= 1 &&
         (left.x != right.x || left.y != right.y);
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
          return HandleReleaseToken(cmd, can_control_all);
        } else if constexpr (std::is_same_v<T, SetTokenOwner>) {
          return HandleSetTokenOwner(cmd, can_control_all);
        } else if constexpr (std::is_same_v<T, RenameToken>) {
          return HandleRenameToken(cmd, can_control_all);
        } else if constexpr (std::is_same_v<T, SetTokenMaxHp>) {
          return HandleSetTokenMaxHp(cmd, actor, can_control_all);
        } else if constexpr (std::is_same_v<T, SetTokenColor>) {
          return HandleSetTokenColor(cmd, can_control_all);
        } else if constexpr (std::is_same_v<T, SetTokenPortrait>) {
          return HandleSetTokenPortrait(cmd, actor, can_control_all);
        } else if constexpr (std::is_same_v<T, SetTokenLight>) {
          return HandleSetTokenLight(cmd, actor, can_control_all);
        } else if constexpr (std::is_same_v<T, UpdateTokenSheet>) {
          return HandleUpdateTokenSheet(cmd, can_control_all);
        } else if constexpr (std::is_same_v<T, UpdateTokenCombat>) {
          return HandleUpdateTokenCombat(cmd, can_control_all);
        } else if constexpr (std::is_same_v<T, StartEncounter>) {
          return HandleStartEncounter(can_control_all);
        } else if constexpr (std::is_same_v<T, EndEncounter>) {
          return HandleEndEncounter(can_control_all);
        } else if constexpr (std::is_same_v<T, SetEncounterComposition>) {
          return HandleSetEncounterComposition(cmd, can_control_all);
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
        } else if constexpr (std::is_same_v<T, FallToken>) {
          return HandleFall(cmd, can_control_all);
        } else if constexpr (std::is_same_v<T, AdvanceTime>) {
          return HandleAdvanceTime(cmd, can_control_all);
        } else if constexpr (std::is_same_v<T, AoeDamage>) {
          return HandleAoeDamage(cmd, can_control_all);
        } else if constexpr (std::is_same_v<T, DeathSave>) {
          return HandleDeathSave(cmd, actor, can_control_all);
        } else if constexpr (std::is_same_v<T, Stabilize>) {
          return HandleStabilize(cmd, can_control_all);
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
        } else if constexpr (std::is_same_v<T, UseTurnAction>) {
          return HandleUseTurnAction(cmd, actor, can_control_all);
        } else if constexpr (std::is_same_v<T, SkillCheck>) {
          return HandleSkillCheck(cmd, can_control_all);
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
  // Персонажи заблокированы, пока их кто-нибудь не заберёт. У монстров и NPC
  // владелец всегда DM, поэтому свободными они не бывают.
  if (token.kind == TokenKind::kPlayer && token.owner.empty()) {
    return false;
  }
  if (token.owner.empty()) {
    return true;
  }
  return SamePlayerName(token.owner, actor);
}

auto GameServer::TurnRejectReason(const Token& token,
                                  bool can_control_all) const -> const char* {
  // Монстры, NPC и ещё не взятые персонажи остаются мастерскими: DM ведёт их
  // без очереди. Взятый DM-ом персонаж играет по правилам игрока.
  if (!FollowsPlayerRules(token, can_control_all)) {
    return nullptr;
  }
  // Недееспособный персонаж не действует ни в свой ход, ни в чужой.
  if (IsIncapacitated(token)) {
    return "персонаж недееспособен";
  }
  if (state_.EncounterState().active && state_.ActiveTokenId() != token.id) {
    return "сейчас не ваш ход";
  }
  return nullptr;
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
  // Взятый DM-ом персонаж ходит по правилам игрока: путь, клетки, бюджет.
  // Монстры, NPC и свободные персонажи для DM по-прежнему без ограничений.
  const bool player_rules = FollowsPlayerRules(*token, can_control_all);
  // В свой ход двигается только тот, чья сейчас очередь; вне боя и у DM
  // ограничений нет. Недееспособного не двигает даже владелец.
  if (const char* reason = TurnRejectReason(*token, can_control_all)) {
    return CommandOutcome{.reject_reason = reason};
  }
  // Игрок не может вслепую ткнуть в неразведанную клетку. DM двигает токены
  // свободно, а разведанная клетка остаётся допустимой, даже если сейчас
  // скрыта туманом.
  if (!can_control_all && !IsExploredTo(actor, command.target)) {
    return CommandOutcome{.reject_reason = "клетка не разведана"};
  }
  // Захваченный, опутанный, парализованный и лежащий без сознания не
  // двигаются: скорость 0 (SRD 5.2.1).
  if (player_rules && IsSpeedZeroed(*token)) {
    return CommandOutcome{.reject_reason = "скорость 0: персонаж обездвижен"};
  }
  // «Испуган»: нельзя добровольно приближаться к источнику страха (2024).
  // Правило не зависит от того, виден ли источник сейчас, — запрещает сам шаг.
  if (const TokenId fear_source = FrightenedSource(*token); fear_source != 0) {
    const Token* source = FindToken(fear_source);
    if (source != nullptr &&
        IsApproaching(token->cell, command.target, source->cell)) {
      return CommandOutcome{
          .reject_reason = "испуган: нельзя приближаться к источнику страха"};
    }
  }

  // Проверяем путь и для игрока вне боя: сквозь стены и живых монстров
  // нельзя, сквозь недееспособных — можно (2024). Свои персонажи друг друга
  // не блокируют, но встать на занятую клетку нельзя. DM по-прежнему двигает
  // токены свободно.
  const std::vector<Cell> blockers = BlockingCells(state_.Tokens(), command.id);
  bool target_occupied = false;
  for (const Token& other : state_.Tokens()) {
    if (other.id != command.id && other.cell == command.target) {
      target_occupied = true;
      break;
    }
  }
  if (player_rules && target_occupied) {
    return CommandOutcome{.reject_reason = "клетка занята"};
  }

  const bool budgeted = state_.EncounterState().active && player_rules;
  const bool prone = IsProne(*token);

  // Прыжок по прямой до 10 футов (в модели нет Силы, это упрощение 2024):
  // стену не перепрыгнуть, пропасть и воду — можно; приземляться в них
  // нельзя, это проверит ApplyCommand.
  if (command.jump) {
    if (prone) {
      return CommandOutcome{.reject_reason =
                                "сначала встаньте из положения «сбит с ног»"};
    }
    const int distance = JumpDistanceCells(token->cell, command.target);
    if (distance <= 0 || distance > kMaxJumpCells) {
      return CommandOutcome{.reject_reason =
                                "прыжок только по прямой до 10 футов"};
    }
    if (!IsJumpPathClear(state_.Map(), token->cell, command.target)) {
      return CommandOutcome{.reject_reason = "стену не перепрыгнуть"};
    }
    if (budgeted && distance > RemainingMovementCells(*token)) {
      return CommandOutcome{.reject_reason = "не хватает движения на прыжок"};
    }
    const std::optional<Event> jump_event = state_.ApplyCommand(command);
    if (!jump_event.has_value()) {
      return CommandOutcome{.reject_reason = "недопустимый прыжок"};
    }
    RecomputeFog();
    CommandOutcome jump_outcome;
    jump_outcome.accepted = true;
    jump_outcome.events.emplace_back(*jump_event);
    if (budgeted) {
      const std::optional<Token> jumped =
          state_.SpendMovement(command.id, distance, token->diagonal_parity);
      if (jumped.has_value()) {
        jump_outcome.events.emplace_back(TokenUpdated{.token = *jumped});
      }
    }
    jump_outcome.events.emplace_back(LogLineAdded{
        .line = LogLine{.author = AuthorName(actor),
                        .text = token->name + " прыгнул на " +
                                std::to_string(distance * 5) + " футов",
                        .timestamp = CurrentTimeSeconds()},
    });
    TryCurrent(command.id, actor, jump_outcome.events);
    TriggerHazards(command.id, kHazardEnter, actor, jump_outcome.events);
    if (token->kind != TokenKind::kPlayer && IsPartyVisible(token->cell)) {
      (void)JoinEncounterIfNeeded(command.id, jump_outcome.events);
    }
    return jump_outcome;
  }

  // Ползание не встаёт: каждый фут пути стоит вдвое. Обычный ход из
  // положения «сбит с ног» сначала платит половину скорости за вставание.
  const bool crawl = prone && command.crawl;
  std::optional<PathCost> path;
  if (player_rules) {
    path = PathCostCells(state_.Map(), token->cell, command.target,
                         token->diagonal_parity, blockers,
                         MovementProfileFor(*token));
    if (!path.has_value()) {
      return CommandOutcome{.reject_reason = "нет пути к клетке"};
    }
  }
  const int path_cost = path.has_value() ? path->cost : 0;
  const int move_cost = crawl ? CrawlCostCells(path_cost) : path_cost;
  const int stand_cost =
      prone && !crawl && budgeted ? StandUpCostCells(*token) : 0;
  if (budgeted) {
    const int remaining = MovementBudgetCells(*token) - token->movement_used;
    if (stand_cost + move_cost > remaining) {
      const char* reason = "не хватает движения на ход";
      if (prone && !crawl) {
        reason = "не хватает движения, чтобы встать";
      } else if (crawl) {
        reason = "не хватает движения, чтобы проползти";
      }
      return CommandOutcome{.reject_reason = reason};
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
  std::optional<Token> updated;
  if (prone && !crawl) {
    updated = state_.RemoveTokenCondition(command.id, std::string(kProne));
  }
  if (budgeted) {
    const int parity =
        path.has_value() ? path->diagonal_parity : token->diagonal_parity;
    updated = state_.SpendMovement(command.id, move_cost + stand_cost, parity);
  }
  if (updated.has_value()) {
    outcome.events.emplace_back(TokenUpdated{.token = std::move(*updated)});
  }
  // Течение срабатывает при входе (и в начале хода — отдельным вызовом).
  // После сдвига урон клетки берётся уже на новом месте.
  TryCurrent(command.id, actor, outcome.events);
  TriggerHazards(command.id, kHazardEnter, actor, outcome.events);
  // Скользкая клетка: при ходе больше половины скорости — проверка Ловкости
  // (Акробатика), провал сбивает с ног. Бонус спасброска — общий, как и в
  // остальных проверках проекта.
  const Token* landed = FindToken(command.id);
  const CellProps& landing =
      state_.Map().PropsAt(landed == nullptr ? command.target : landed->cell);
  if ((landing.flags & kCellSlippery) != 0 && landing.slippery_dc > 0 &&
      !prone && move_cost > std::max(1, EffectiveSpeedCells(*token) / 2)) {
    std::uniform_int_distribution<int> d20(1, 20);
    const int roll = d20(rng_) + token->save_bonus - ExhaustionPenalty(*token);
    const bool success = roll >= landing.slippery_dc;
    outcome.events.emplace_back(LogLineAdded{
        .line = LogLine{.author = AuthorName(actor),
                        .text = token->name + ": проверка Ловкости " +
                                std::to_string(roll) + " против " +
                                std::to_string(landing.slippery_dc) +
                                (success ? " — удержался" : " — сбит с ног"),
                        .timestamp = CurrentTimeSeconds()},
    });
    if (!success) {
      if (std::optional<Token> slipped = state_.AddTokenCondition(
              command.id, Condition(std::string(kProne)))) {
        outcome.events.emplace_back(TokenUpdated{.token = std::move(*slipped)});
      }
    }
  }
  // Подошедшего к столу монстра замечают: он вступает в идущий бой. Это
  // случай «приближение»; шум и решение DM идут через состав боя.
  if (token->kind != TokenKind::kPlayer && IsPartyVisible(token->cell)) {
    (void)JoinEncounterIfNeeded(command.id, outcome.events);
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
  if (!IsValidHitDie(command.hit_die)) {
    return CommandOutcome{.reject_reason = "некорректная кость хитов"};
  }
  if (command.con_modifier < -kMaxConModifier ||
      command.con_modifier > kMaxConModifier) {
    return CommandOutcome{.reject_reason =
                              "некорректный модификатор Телосложения"};
  }
  if (command.hit_dice_count < 0 || command.hit_dice_count > 1000) {
    return CommandOutcome{.reject_reason = "некорректное число костей хитов"};
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
    DamageType damage_type = DamageType::kNone;
    if (!ParseDamageProfile(command.damage, damage, damage_type)) {
      return CommandOutcome{.reject_reason = "некорректное выражение урона"};
    }
  }
  if (command.attack_range < 0 || command.attack_range > kMaxSpeed) {
    return CommandOutcome{.reject_reason = "некорректная дистанция атаки"};
  }
  if (!IsValidDamageTypeBit(command.resist) ||
      !IsValidDamageTypeBit(command.vulnerable) ||
      !IsValidDamageTypeBit(command.immune)) {
    return CommandOutcome{.reject_reason = "некорректная защита от урона"};
  }
  std::string owner = command.owner;
  if (owner.size() > kMaxTokenName) {
    return CommandOutcome{.reject_reason = "имя владельца слишком длинное"};
  }
  if (owner.empty() && command.kind != TokenKind::kPlayer) {
    owner = std::string(kDmOwner);
  }

  const bool encounter_active = state_.EncounterState().active;
  const TokenId id = state_.AddToken(
      name, command.cell, std::move(owner), command.kind, command.max_hp, 0,
      command.ac, command.speed, command.notes, command.attack_bonus,
      command.save_bonus, command.damage, command.attack_range, command.resist,
      command.vulnerable, command.immune, command.hit_die, command.con_modifier,
      command.hit_dice_count, command.auto_hp);
  const std::optional<Token> token = state_.TokenCopy(id);
  if (!token.has_value()) {
    return CommandOutcome{.reject_reason = "не удалось создать токен"};
  }
  CommandOutcome outcome;
  outcome.accepted = true;
  outcome.events.emplace_back(TokenAdded{.token = *token});
  if (encounter_active) {
    // Новый токен получает настоящий бросок, а не нулевую инициативу.
    (void)JoinEncounterIfNeeded(id, outcome.events);
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
  // Удар по токену, который ещё не в бою, втягивает его: он бросает
  // инициативу и встаёт в конец раунда.
  (void)JoinEncounterIfNeeded(command.id, outcome.events);
  DamageAndLog(command.id, command.amount, false, command.damage_type, actor,
               outcome.events);
  return outcome;
}

auto GameServer::DamageAndLog(TokenId id, int amount, bool critical,
                              DamageType damage_type, const std::string& actor,
                              std::vector<Event>& events) -> void {
  const Token* token = FindToken(id);
  if (token == nullptr) {
    return;
  }
  const std::string name = token->name;
  // Сопротивление, уязвимость и иммунитет цели (SRD 5.2.1).
  DamageProfile profile = EffectiveDamageProfile(*token);
  // Полностью погружённый получает сопротивление огню (2024). Погружением
  // считаем глубокую воду: слоя плавания над мелководьем нет.
  bool submerged_fire = false;
  if (damage_type == DamageType::kFire) {
    const CellProps& target_props = state_.Map().PropsAt(token->cell);
    if (target_props.water == WaterDepth::kDeep ||
        state_.Map().TerrainAt(token->cell) == Terrain::kWater) {
      profile.resist |= DamageTypeBit(DamageType::kFire);
      submerged_fire = true;
    }
  }
  const int dealt = AdjustedDamage(amount, damage_type, profile);
  const std::uint32_t type_bit =
      damage_type == DamageType::kNone ? 0 : DamageTypeBit(damage_type);
  const bool immune = type_bit != 0 && (profile.immune & type_bit) != 0;
  const bool resisted = type_bit != 0 && (profile.resist & type_bit) != 0;
  const bool vulnerable = type_bit != 0 && (profile.vulnerable & type_bit) != 0;

  std::ostringstream text;
  text << name << " получил " << dealt << " урона";
  if (damage_type != DamageType::kNone) {
    text << " (" << DamageTypeName(damage_type) << ")";
  }
  if (resisted && !immune) {
    text << "; сопротивление";
    if (submerged_fire) {
      text << " огню (под водой)";
    }
  }
  if (vulnerable && !immune) {
    text << "; уязвимость";
  }
  if (immune) {
    text << "; иммунитет";
  }
  if (dealt <= 0) {
    events.emplace_back(LogLineAdded{
        .line = LogLine{.author = AuthorName(actor),
                        .text = text.str(),
                        .timestamp = CurrentTimeSeconds()},
    });
    return;
  }
  const int temp_before = token->temp_hp;
  const int hp_before = token->hp;
  const int save_bonus = token->save_bonus - ExhaustionPenalty(*token);
  const bool was_dead = HasCondition(*token, kDead);
  const bool concentrating = HasCondition(*token, kConcentration);
  std::optional<Token> damaged = state_.DamageToken(id, dealt, critical);
  if (!damaged.has_value()) {
    return;
  }
  Token current = std::move(*damaged);
  // Любой прошедший урон выдаёт скрытного: скрытность снимается.
  if (current.hidden) {
    if (const std::optional<Token> unhidden = state_.SetTokenHidden(id, 0);
        unhidden.has_value()) {
      current = *unhidden;
    }
  }
  // Смерть, 0 HP и недееспособность рвут концентрацию без спасброска.
  bool concentration_broken = false;
  if (concentrating && IsIncapacitated(current)) {
    if (std::optional<Token> without =
            state_.RemoveTokenCondition(id, kConcentration)) {
      current = std::move(*without);
      concentration_broken = true;
    }
  }
  const bool died = !was_dead && HasCondition(current, kDead);
  const int absorbed = std::min(temp_before, dealt);
  if (absorbed > 0) {
    text << " (" << absorbed << " из временных HP)";
  }
  if (died) {
    text << "; " << name << " погиб";
  } else if (hp_before > 0 && current.hp == 0) {
    text << "; " << name << " без сознания";
  }
  events.emplace_back(TokenUpdated{.token = current});
  events.emplace_back(LogLineAdded{
      .line = LogLine{.author = AuthorName(actor),
                      .text = text.str(),
                      .timestamp = CurrentTimeSeconds()},
  });
  if (!concentrating) {
    return;
  }
  if (concentration_broken) {
    events.emplace_back(LogLineAdded{
        .line = LogLine{.author = AuthorName(actor),
                        .text = name + ": концентрация сорвана",
                        .timestamp = CurrentTimeSeconds()},
    });
    return;
  }
  // Проверка концентрации: d20 + бонус спасброска против 10 или половины
  // полученного урона, но не выше 30. Провал снимает состояние
  // «концентрация». Бонус спасброска в листе один на все спасброски —
  // упрощение проекта (см. AGENTS.md).
  const int dc = ConcentrationDc(dealt);
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

auto GameServer::TriggerHazards(TokenId id, std::uint8_t trigger,
                                const std::string& actor,
                                std::vector<Event>& events) -> void {
  const Token* token = FindToken(id);
  if (token == nullptr) {
    return;
  }
  const Cell cell = token->cell;
  const HazardProfile* hazard = state_.Map().HazardAt(cell);
  if (hazard == nullptr || hazard->damage.empty() ||
      (hazard->triggers & trigger) == 0) {
    return;
  }
  const std::uint8_t index = state_.Map().PropsAt(cell).hazard;
  if (std::ranges::find(token->hazards_done, index) !=
      token->hazards_done.end()) {
    return;
  }
  DiceExpression expression;
  if (!ParseDiceExpression(hazard->damage, expression)) {
    return;
  }
  const DiceRollResult roll = RollDiceExpression(expression, rng_);
  int damage = std::max(0, roll.total);
  std::ostringstream text;
  text << token->name << ": свойство клетки " << hazard->damage << " = "
       << damage;
  if (damage > 0 && hazard->save_dc > 0) {
    std::uniform_int_distribution<int> d20(1, 20);
    const int save_bonus = token->save_bonus - ExhaustionPenalty(*token);
    const int save = d20(rng_) + save_bonus;
    const bool success = save >= hazard->save_dc;
    if (success && hazard->half_on_success) {
      damage /= 2;
    }
    text << ", спасбросок " << save << " против " << hazard->save_dc
         << (success ? " — успех" : " — провал") << ", " << damage << " урона";
  }
  if (std::optional<Token> marked = state_.MarkHazardDone(id, index)) {
    events.emplace_back(TokenUpdated{.token = std::move(*marked)});
  }
  events.emplace_back(LogLineAdded{
      .line = LogLine{.author = AuthorName(actor),
                      .text = text.str(),
                      .timestamp = CurrentTimeSeconds()},
  });
  if (damage > 0) {
    DamageAndLog(id, damage, false, hazard->type, actor, events);
  }
}

auto GameServer::TryCurrent(TokenId id, const std::string& actor,
                            std::vector<Event>& events) -> void {
  const Token* token = FindToken(id);
  if (token == nullptr) {
    return;
  }
  const CellProps& props = state_.Map().PropsAt(token->cell);
  if (props.current_dir == 0 || props.current_strength == 0) {
    return;
  }
  const std::string name = token->name;
  const int dc = CurrentDc(props.current_strength);
  std::uniform_int_distribution<int> d20(1, 20);
  const int roll = d20(rng_) + token->save_bonus - ExhaustionPenalty(*token);
  if (roll >= dc) {
    return;
  }
  const Cell offset = CurrentDirectionOffset(props.current_dir);
  int shifted = 0;
  for (int step = 0; step < CurrentShiftCells(props.current_strength); ++step) {
    const Token* moving = FindToken(id);
    if (moving == nullptr) {
      break;
    }
    const Cell next{.x = moving->cell.x + offset.x,
                    .y = moving->cell.y + offset.y};
    if (!state_.Map().Contains(next) || state_.Map().IsBlocked(next)) {
      break;
    }
    bool occupied = false;
    for (const Token& other : state_.Tokens()) {
      if (other.id != id && other.cell == next) {
        occupied = true;
        break;
      }
    }
    if (occupied) {
      break;
    }
    const std::optional<Event> moved =
        state_.ApplyCommand(MoveToken{.id = id, .target = next});
    if (!moved.has_value()) {
      break;
    }
    events.emplace_back(*moved);
    ++shifted;
  }
  RecomputeFog();
  std::ostringstream text;
  text << name << ": проверка Силы против течения " << roll << " против " << dc
       << " — течение";
  if (shifted > 0) {
    text << " сдвинуло на " << shifted << " кл.";
  } else {
    text << " прижало к препятствию";
  }
  events.emplace_back(LogLineAdded{
      .line = LogLine{.author = AuthorName(actor),
                      .text = text.str(),
                      .timestamp = CurrentTimeSeconds()},
  });
  if (shifted > 0) {
    TriggerHazards(id, kHazardEnter, actor, events);
  }
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
  // Очарованный не атакует очаровавшего (SRD 5.2.1).
  if (IsCharmedAgainst(*attacker, target->id)) {
    return CommandOutcome{.reject_reason =
                              "очарован: нельзя атаковать очаровавшего"};
  }
  const bool encounter_active = state_.EncounterState().active;
  const bool own_turn =
      !encounter_active || state_.ActiveTokenId() == attacker->id;
  const bool is_reaction = encounter_active && !own_turn;
  // Взятый DM-ом персонаж бьёт по правилам игрока; остальные мастерские токены
  // DM — без счёта действий и реакций.
  const bool player_rules = FollowsPlayerRules(*attacker, can_control_all);
  if (player_rules) {
    if (IsIncapacitated(*attacker)) {
      return CommandOutcome{.reject_reason = "персонаж недееспособен"};
    }
    if (is_reaction) {
      if (!command.reaction) {
        return CommandOutcome{.reject_reason = "сейчас не ваш ход"};
      }
      if (attacker->reaction_used) {
        return CommandOutcome{.reject_reason = "реакция уже потрачена"};
      }
      if (!AreAdjacent(attacker->cell, target->cell)) {
        return CommandOutcome{.reject_reason =
                                  "реакция — только по соседней цели"};
      }
    } else if (attacker->action_used) {
      return CommandOutcome{.reject_reason = "действие уже потрачено"};
    }
  }
  // Игрок не бьёт то, чего не видит: иначе командой вручную можно нащупать
  // монстра в тумане. DM видит весь стол. Ослеплённый бьёт вслепую (помеху
  // даёт состояние): карта у него чёрная, но соседа он знает на ощупь.
  if (!can_control_all && !HasCondition(*attacker, kBlinded) &&
      !IsVisibleTo(actor, target->cell)) {
    return CommandOutcome{.reject_reason = "цель не видна"};
  }
  // Скрытную цель игрок не выбирает, пока её не выдало пассивное Восприятие.
  if (!can_control_all && target->hidden && !NoticesHidden(actor, *target)) {
    return CommandOutcome{.reject_reason = "цель не видна"};
  }
  if (command.advantage && command.disadvantage) {
    return CommandOutcome{.reject_reason =
                              "нельзя одновременно преимущество и помеху"};
  }
  const int distance_cells =
      std::max(std::abs(attacker->cell.x - target->cell.x),
               std::abs(attacker->cell.y - target->cell.y));
  const int attack_range = attacker->attack_range;
  if (!IsAttackInReach(attack_range, distance_cells)) {
    return CommandOutcome{.reject_reason = attack_range > 0
                                               ? "цель за пределами дистанции"
                                               : "цель дальше 1 клетки"};
  }
  // Дальнобойная атака в 5 футах от другого противника идёт с помехой.
  // Враждебность в токенах не хранится, поэтому учитывается любой сосед,
  // кроме самой цели: упрощение проекта.
  bool threatened = false;
  if (attack_range > 0) {
    for (const Token& other : state_.Tokens()) {
      if (other.id != attacker->id && other.id != target->id &&
          AreAdjacent(other.cell, attacker->cell)) {
        threatened = true;
        break;
      }
    }
  }
  // Преимущество и помеха от состояний 2024 складываются с ручными и взаимно
  // гасятся (SRD 5.2.1). Взгляд через дым и тьму считается по свету сцены.
  // Скрытный атакующий невидим для цели, скрытную цель не видно атакующему;
  // «Помощь» союзнику даёт преимущество на эту атаку.
  const bool sees_target =
      CanSeeForAttack(state_.Map(), light_, *attacker, target->cell) &&
      !target->hidden;
  const bool target_sees_attacker =
      CanSeeForAttack(state_.Map(), light_, *target, attacker->cell) &&
      !attacker->hidden;
  // «Испуган» даёт помеху, только пока источник страха в поле зрения.
  bool frightened_applies = true;
  if (const TokenId fear_source = FrightenedSource(*attacker);
      fear_source != 0) {
    const Token* source = FindToken(fear_source);
    frightened_applies =
        source != nullptr &&
        CanSeeCell(state_.Map(), light_, *attacker, source->cell);
  }
  const AttackRollModes modes = AttackRollModesFor(
      *attacker, *target, distance_cells, attack_range, threatened, sees_target,
      target_sees_attacker, frightened_applies);
  bool advantage = command.advantage || modes.advantage || attacker->helped;
  bool disadvantage = command.disadvantage || modes.disadvantage;
  // Под водой дальнобойная атака идёт с помехой в обычной дистанции и
  // автоматически промахивается за ней (SRD 5.2.1). Погружением считаем
  // глубокую воду: слоя плавания над мелководьем нет.
  const CellProps& attacker_props = state_.Map().PropsAt(attacker->cell);
  const bool submerged =
      attacker_props.water == WaterDepth::kDeep ||
      state_.Map().TerrainAt(attacker->cell) == Terrain::kWater;
  bool underwater_miss = false;
  if (submerged && attack_range > 0 && distance_cells > attack_range) {
    underwater_miss = true;
  } else if (submerged && attack_range > 0) {
    disadvantage = true;
  }
  if (advantage && disadvantage) {
    advantage = false;
    disadvantage = false;
  }
  DiceExpression damage;
  DamageType damage_type = DamageType::kNone;
  if (attacker->damage.size() > kMaxDamageExpression ||
      (!attacker->damage.empty() &&
       !ParseDamageProfile(attacker->damage, damage, damage_type))) {
    return CommandOutcome{.reject_reason = "некорректное выражение урона"};
  }
  // Укрытие по стороне клетки: половина +2 к КД, три четверти +5, полное
  // укрытие не даёт выбрать цель напрямую (область при этом действует).
  // Существо на линии даёт только половину укрытия; лежащие без сознания не
  // считаются, цель и атакующий — тоже. Размеров в модели нет, поэтому любое
  // живое существо считается достаточным (упрощение 2024).
  std::vector<Cell> creatures;
  for (const Token& other : state_.Tokens()) {
    if (other.id != attacker->id && other.id != target->id &&
        !IsIncapacitated(other)) {
      creatures.push_back(other.cell);
    }
  }
  const CoverLevel cover =
      CoverBetween(state_.Map(), attacker->cell, target->cell, creatures);
  if (cover == CoverLevel::kFull) {
    return CommandOutcome{.reject_reason = "цель за полным укрытием"};
  }
  const int cover_bonus = CoverBonus(cover);
  const int effective_ac = target->ac + cover_bonus;

  // Истощение: −2 ко всем d20 за уровень.
  const int exhaustion = ExhaustionPenalty(*attacker);
  const int effective_bonus = attacker->attack_bonus - exhaustion;

  const DiceExpression attack{
      .count = 1,
      .sides = 20,
      .modifier = effective_bonus,
      .advantage = advantage,
      .disadvantage = disadvantage,
  };
  const DiceRollResult roll = RollDiceExpression(attack, rng_);
  const AttackCheck check =
      CheckAttack(roll.chosen, effective_bonus, effective_ac);
  const bool critical =
      !underwater_miss &&
      (check.critical ||
       (check.hit && IsAutoCriticalOnHit(*target, distance_cells)));
  std::ostringstream text;
  text << "Атака" << (is_reaction ? " (реакция)" : "") << ": " << attacker->name
       << " по " << target->name;
  if (advantage) {
    text << " с преимуществом";
  } else if (disadvantage) {
    text << " с помехой";
  }
  if (exhaustion > 0) {
    text << " (истощение −" << exhaustion << ")";
  }
  text << ": d20" << (effective_bonus >= 0 ? " + " : " - ")
       << std::abs(effective_bonus) << " = " << roll.breakdown;
  if (roll.breakdown != std::to_string(roll.total)) {
    text << " = " << roll.total;
  }
  text << " против КД " << effective_ac;
  if (cover_bonus > 0) {
    text << " (укрытие +" << cover_bonus << ")";
  }

  CommandOutcome outcome;
  outcome.accepted = true;
  // Даже промах — это шум: цель, которая ещё не в бою, вступает в него.
  (void)JoinEncounterIfNeeded(command.target, outcome.events);
  int applied_damage = 0;
  const TokenId target_id = target->id;
  if (underwater_miss) {
    text << " — промах (под водой за обычной дистанцией)";
  } else if (!check.hit) {
    text << " — промах";
  } else if (attacker->damage.empty()) {
    text << " — попадание, урон не задан";
  } else {
    const DiceRollResult damage_roll = RollAttackDamage(damage, critical, rng_);
    text << (critical ? " — критическое попадание, урон "
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
  // «Помощь» и скрытность тратятся в любом случае: бросок уже состоялся.
  const bool was_helped = attacker->helped;
  const bool was_hidden = attacker->hidden;
  if (was_helped) {
    (void)state_.ClearTokenHelped(attacker->id);
  }
  if (was_hidden) {
    (void)state_.SetTokenHidden(attacker->id, 0);
  }
  if (player_rules && encounter_active) {
    // Атака исполняет отложенное действие и расходует действие или реакцию.
    const std::optional<Token> spent = is_reaction
                                           ? state_.SpendReaction(attacker->id)
                                           : state_.SpendAction(attacker->id);
    if (spent.has_value()) {
      outcome.events.emplace_back(TokenUpdated{.token = *spent});
    }
  } else if (was_helped || was_hidden) {
    if (const std::optional<Token> current = state_.TokenCopy(attacker->id);
        current.has_value()) {
      outcome.events.emplace_back(TokenUpdated{.token = *current});
    }
  }
  if (applied_damage > 0) {
    DamageAndLog(target_id, applied_damage, critical, damage_type, actor,
                 outcome.events);
  }
  return outcome;
}

auto GameServer::HandleUseTurnAction(const UseTurnAction& command,
                                     const std::string& actor,
                                     bool can_control_all) -> CommandOutcome {
  const Token* token = FindToken(command.id);
  if (token == nullptr) {
    return CommandOutcome{.reject_reason = "токен не найден"};
  }
  if (!CanControl(*token, actor, can_control_all)) {
    return CommandOutcome{.reject_reason = ControlRejectReason(*token)};
  }
  if (!IsValidTurnAction(command.kind)) {
    return CommandOutcome{.reject_reason = "неизвестное действие"};
  }
  if (const char* reason = TurnRejectReason(*token, can_control_all)) {
    return CommandOutcome{.reject_reason = reason};
  }
  if (FollowsPlayerRules(*token, can_control_all) && token->action_used) {
    return CommandOutcome{.reject_reason = "действие уже потрачено"};
  }

  // Цель «Помощи» проверяется до траты действия: промах по клетке не должен
  // стоить хода.
  TokenId help_target = 0;
  std::string help_name;
  if (command.kind == TurnActionKind::kHelp) {
    const Token* ally = FindToken(command.target);
    if (ally == nullptr || ally->id == token->id) {
      return CommandOutcome{.reject_reason = "выберите союзника под курсором"};
    }
    if (!AreAdjacent(token->cell, ally->cell)) {
      return CommandOutcome{.reject_reason =
                                "помощь — только соседнему союзнику"};
    }
    help_target = ally->id;
    help_name = ally->name;
  }
  const int dc =
      command.dc > 0 ? command.dc : static_cast<int>(kDefaultSkillDc);
  if ((command.kind == TurnActionKind::kSearch ||
       command.kind == TurnActionKind::kStudy) &&
      dc > kMaxSaveDc) {
    return CommandOutcome{.reject_reason = "некорректная КС проверки"};
  }

  const std::string name = token->name;
  const bool helped_before = token->helped;
  const std::optional<Token> acted =
      state_.UseTurnAction(command.id, command.kind, help_target);
  if (!acted.has_value()) {
    return CommandOutcome{.reject_reason = "не удалось выполнить действие"};
  }

  CommandOutcome outcome;
  outcome.accepted = true;
  std::vector<TokenId> changed{command.id};
  std::vector<std::string> extra_lines;
  std::string effect = TurnActionEffect(command.kind, *acted);

  switch (command.kind) {
    case TurnActionKind::kHelp:
      effect = " — помогает " + help_name +
               ": преимущество на следующую атаку или проверку";
      changed.push_back(help_target);
      break;
    case TurnActionKind::kHide: {
      // Скрытность против КС 15; преимущество от «Помощи» тратится.
      const int bonus = SkillCheckBonus(*acted, Skill::kStealth);
      const DiceExpression check{.count = 1,
                                 .sides = 20,
                                 .modifier = bonus,
                                 .advantage = helped_before};
      const DiceRollResult roll = RollDiceExpression(check, rng_);
      (void)state_.ClearTokenHelped(command.id);
      const bool success = roll.total >= dc;
      (void)state_.SetTokenHidden(command.id, success ? roll.total : 0);
      effect = " — Скрытность " + FormatD20Roll(bonus, roll) + " против КС " +
               std::to_string(dc) + ": " +
               std::string(CheckOutcomeWord(roll.total, dc));
      break;
    }
    case TurnActionKind::kSearch: {
      // Восприятие ищет скрытных существ в 30 футах: результат выше их
      // Скрытности снимает скрытность.
      const int bonus = SkillCheckBonus(*acted, Skill::kPerception);
      const DiceExpression check{.count = 1,
                                 .sides = 20,
                                 .modifier = bonus,
                                 .advantage = helped_before};
      const DiceRollResult roll = RollDiceExpression(check, rng_);
      (void)state_.ClearTokenHelped(command.id);
      std::ostringstream line;
      line << " — Восприятие " << FormatD20Roll(bonus, roll) << " против КС "
           << dc << ": " << CheckOutcomeWord(roll.total, dc);
      for (const Token& other : state_.Tokens()) {
        if (other.id == command.id || !other.hidden) {
          continue;
        }
        const int distance = std::max(std::abs(other.cell.x - acted->cell.x),
                                      std::abs(other.cell.y - acted->cell.y));
        if (distance > kSearchRadiusCells || other.stealth_total > roll.total) {
          continue;
        }
        (void)state_.SetTokenHidden(other.id, 0);
        changed.push_back(other.id);
        extra_lines.push_back(name + " замечает: " + other.name);
      }
      effect = line.str();
      break;
    }
    case TurnActionKind::kStudy: {
      const int bonus = SkillCheckBonus(*acted, Skill::kInvestigation);
      const DiceExpression check{.count = 1,
                                 .sides = 20,
                                 .modifier = bonus,
                                 .advantage = helped_before};
      const DiceRollResult roll = RollDiceExpression(check, rng_);
      (void)state_.ClearTokenHelped(command.id);
      effect = " — Расследование " + FormatD20Roll(bonus, roll) +
               " против КС " + std::to_string(dc) + ": " +
               std::string(CheckOutcomeWord(roll.total, dc));
      break;
    }
    case TurnActionKind::kDash:
    case TurnActionKind::kDisengage:
    case TurnActionKind::kDodge:
    case TurnActionKind::kUseObject:
    case TurnActionKind::kReady:
      break;
  }
  // Скрытность и помощь могли измениться: стол получает итоговые токены.
  for (const TokenId id : changed) {
    if (const std::optional<Token> current = state_.TokenCopy(id);
        current.has_value()) {
      outcome.events.emplace_back(TokenUpdated{.token = *current});
    }
  }
  outcome.events.emplace_back(LogLineAdded{
      .line =
          LogLine{.author = AuthorName(actor),
                  .text = name + ": " + TurnActionName(command.kind) + effect,
                  .timestamp = CurrentTimeSeconds()},
  });
  for (std::string& line : extra_lines) {
    outcome.events.emplace_back(LogLineAdded{
        .line = LogLine{.author = AuthorName(actor),
                        .text = std::move(line),
                        .timestamp = CurrentTimeSeconds()},
    });
  }
  return outcome;
}

auto GameServer::HandleSkillCheck(const SkillCheck& command,
                                  bool can_control_all) -> CommandOutcome {
  if (!can_control_all) {
    return CommandOutcome{.reject_reason = "проверки бросает только DM"};
  }
  const Token* token = FindToken(command.id);
  if (token == nullptr) {
    return CommandOutcome{.reject_reason = "токен не найден"};
  }
  if (!IsValidSkill(command.skill)) {
    return CommandOutcome{.reject_reason = "неизвестный навык"};
  }
  if (command.dc <= 0 || command.dc > kMaxSaveDc) {
    return CommandOutcome{.reject_reason = "некорректная КС проверки"};
  }
  const bool helped = token->helped;
  const int bonus = SkillCheckBonus(*token, command.skill);
  const DiceExpression expression{
      .count = 1, .sides = 20, .modifier = bonus, .advantage = helped};
  const DiceRollResult roll = RollDiceExpression(expression, rng_);

  CommandOutcome outcome;
  outcome.accepted = true;
  if (helped) {
    if (const std::optional<Token> cleared =
            state_.ClearTokenHelped(command.id);
        cleared.has_value()) {
      outcome.events.emplace_back(TokenUpdated{.token = *cleared});
    }
  }
  std::ostringstream text;
  text << "Проверка (" << SkillName(command.skill) << "): " << token->name;
  if (helped) {
    text << " с преимуществом";
  }
  text << ", " << FormatD20Roll(bonus, roll) << " против КС " << command.dc
       << " — " << CheckOutcomeWord(roll.total, command.dc);
  outcome.events.emplace_back(LogLineAdded{
      .line = LogLine{.author = "DM",
                      .text = text.str(),
                      .timestamp = CurrentTimeSeconds()},
  });
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
  if (command.hit_dice < 0 || command.hit_dice > kMaxLevel) {
    return CommandOutcome{.reject_reason = "некорректное число костей хитов"};
  }
  // Инициатива прерывает отдых: в бою стол не отдыхает.
  if (state_.EncounterState().active) {
    return CommandOutcome{.reject_reason =
                              "в бою отдых прерывает инициатива: сначала бой"};
  }
  if (command.long_rest) {
    if (const int ready_in = state_.LongRestReadyInSeconds(); ready_in > 0) {
      return CommandOutcome{.reject_reason =
                                "следующий длинный отдых не раньше чем через " +
                                FormatConditionDuration(ready_in)};
    }
  }
  const bool has_resting_players =
      std::ranges::any_of(state_.Tokens(), [](const Token& token) -> bool {
        return token.kind == TokenKind::kPlayer && token.hp > 0 &&
               !HasCondition(token, kDead);
      });
  if (!has_resting_players) {
    return CommandOutcome{
        .reject_reason = "лежачим на 0 HP отдых не помогает: сначала лечение"};
  }
  const std::vector<RestChange> changed =
      state_.RestParty(command.long_rest, rng_, command.hit_dice);
  CommandOutcome outcome;
  outcome.accepted = true;
  for (const RestChange& change : changed) {
    outcome.events.emplace_back(TokenUpdated{.token = change.token});
  }
  std::vector<std::string> downed;
  for (const Token& token : state_.Tokens()) {
    if (token.kind == TokenKind::kPlayer && token.hp == 0 &&
        !HasCondition(token, kDead)) {
      downed.push_back(token.name);
    }
  }
  std::string text;
  if (command.long_rest) {
    int exhaustion_removed = 0;
    for (const RestChange& change : changed) {
      exhaustion_removed += change.exhaustion_removed;
    }
    text = "Длинный отдых (8 часов): HP и кости хитов восстановлены";
    if (exhaustion_removed > 0) {
      text += ", истощение -" + std::to_string(exhaustion_removed);
    }
    if (!downed.empty()) {
      text += "; на 0 HP не отдыхают: " + JoinChanges(downed);
    }
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
    text = "Короткий отдых (1 час";
    if (command.hit_dice > 0) {
      text += ", до " + std::to_string(command.hit_dice) + " " +
              std::string(HitDiceWord(command.hit_dice));
    }
    text += "): ";
    text += parts.empty() ? "кости хитов не тратились" : JoinChanges(parts);
    if (!downed.empty()) {
      text += "; на 0 HP не отдыхают: " + JoinChanges(downed);
    }
  }
  outcome.events.emplace_back(LogLineAdded{
      .line =
          LogLine{
              .author = "DM", .text = text, .timestamp = CurrentTimeSeconds()},
  });
  return outcome;
}

auto GameServer::HandleFall(const FallToken& command, bool can_control_all)
    -> CommandOutcome {
  if (!can_control_all) {
    return CommandOutcome{.reject_reason = "падение объявляет только DM"};
  }
  const Token* token = FindToken(command.id);
  if (token == nullptr) {
    return CommandOutcome{.reject_reason = "токен не найден"};
  }
  if (command.feet <= 0 || command.feet > kMaxFallFeet) {
    return CommandOutcome{.reject_reason = "некорректная высота падения"};
  }
  const int dice = FallDamageDice(command.feet);
  std::uniform_int_distribution<int> die(1, 6);
  int damage = 0;
  std::ostringstream breakdown;
  breakdown << dice << "d6";
  if (dice > 0) {
    breakdown << " =";
    for (int i = 0; i < dice; ++i) {
      const int roll = die(rng_);
      damage += roll;
      breakdown << " " << roll;
    }
    breakdown << " = " << damage;
  }
  const std::string name = token->name;
  CommandOutcome outcome;
  outcome.accepted = true;
  outcome.events.emplace_back(LogLineAdded{
      .line =
          LogLine{.author = "DM",
                  .text = name + " падает с " + std::to_string(command.feet) +
                          " футов: " + breakdown.str(),
                  .timestamp = CurrentTimeSeconds()},
  });
  // Сопротивление и иммунитет считаются до падения: если урона не осталось,
  // персонаж не получает «сбит с ног».
  const int dealt = AdjustedDamage(damage, DamageType::kBludgeoning,
                                   EffectiveDamageProfile(*token));
  if (dealt > 0) {
    DamageAndLog(command.id, damage, false, DamageType::kBludgeoning, "DM",
                 outcome.events);
    const std::optional<Token> prone =
        state_.AddTokenCondition(command.id, Condition{std::string(kProne)});
    if (prone.has_value()) {
      outcome.events.emplace_back(TokenUpdated{.token = *prone});
    }
  }
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
  header << "Область: " << command.expression << " = " << total_damage;
  if (command.damage_type != DamageType::kNone) {
    header << " " << DamageTypeName(command.damage_type);
  }
  header << ", спасбросок " << command.save_dc;
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
    // Область задевает и тех, кто ещё не в бою: шум втягивает их.
    (void)JoinEncounterIfNeeded(token.id, outcome.events);
    const std::string name = token.name;
    const int cover_bonus =
        CoverBonus(CoverBetween(state_.Map(), aoe.origin, token.cell));
    const int save_bonus =
        token.save_bonus - ExhaustionPenalty(token) + cover_bonus;
    const int roll = d20(rng_) + save_bonus;
    const bool success = roll >= command.save_dc;
    const DamageProfile profile = EffectiveDamageProfile(token);
    // Сопротивление, уязвимость и иммунитет применяются после половинного
    // урона за успешный спасбросок.
    const int damage = AdjustedDamage(success ? total_damage / 2 : total_damage,
                                      command.damage_type, profile);
    if (damage > 0) {
      if (std::optional<Token> updated = state_.DamageToken(token.id, damage)) {
        outcome.events.emplace_back(TokenUpdated{.token = std::move(*updated)});
      }
    }
    std::ostringstream line;
    line << name << ": спасбросок " << roll << " против " << command.save_dc
         << (success ? " — успех, " : " — провал, ") << damage << " урона";
    if (cover_bonus > 0) {
      line << " (укрытие +" << cover_bonus << ")";
    }
    const std::uint32_t type_bit = command.damage_type == DamageType::kNone
                                       ? 0
                                       : DamageTypeBit(command.damage_type);
    if (type_bit != 0 && (profile.immune & type_bit) != 0) {
      line << " (иммунитет)";
    } else if (type_bit != 0) {
      if ((profile.resist & type_bit) != 0) {
        line << " (сопротивление)";
      }
      if ((profile.vulnerable & type_bit) != 0) {
        line << " (уязвимость)";
      }
    }
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
  if (HasCondition(*token, kDead)) {
    return CommandOutcome{.reject_reason = "персонаж уже мёртв"};
  }
  if (HasCondition(*token, kStable) || token->death_save_successes >= 3) {
    return CommandOutcome{.reject_reason =
                              "персонаж стабилен, спасброски не нужны"};
  }
  CommandOutcome outcome;
  outcome.accepted = true;
  const std::optional<Token> updated =
      RollDeathSave(command.id, AuthorName(actor), false, outcome.events);
  if (!updated.has_value()) {
    return CommandOutcome{.reject_reason = "не удалось бросить спасбросок"};
  }
  outcome.events.emplace_back(TokenUpdated{.token = *updated});
  return outcome;
}

auto GameServer::RollDeathSave(TokenId id, std::string_view author,
                               bool automatic, std::vector<Event>& events)
    -> std::optional<Token> {
  const std::optional<Token> token = state_.TokenCopy(id);
  if (!token.has_value() || token->hp > 0 || HasCondition(*token, kDead) ||
      HasCondition(*token, kStable) || token->death_save_successes >= 3) {
    return std::nullopt;
  }
  std::uniform_int_distribution<int> d20(1, 20);
  const int roll = d20(rng_);
  const int penalty = ExhaustionPenalty(*token);
  const int stable_seconds = RollStableSeconds();
  std::optional<Token> updated =
      state_.DeathSave(id, roll, penalty, stable_seconds);
  if (!updated.has_value()) {
    return std::nullopt;
  }
  std::ostringstream text;
  text << token->name << ": спасбросок от смерти";
  if (automatic) {
    text << " (начало хода)";
  }
  text << " " << roll;
  const int effective = roll - penalty;
  if (penalty > 0 && roll != 1 && roll != 20) {
    text << " − " << penalty << " (истощение) = " << effective;
  }
  if (roll == 20) {
    text << " — критический успех, 1 HP";
  } else if (roll == 1) {
    text << " — критический провал (два), " << updated->death_save_failures
         << "/3";
    if (updated->death_save_failures >= 3) {
      text << ", персонаж погиб";
    }
  } else if (effective >= 10) {
    text << " — успех (" << updated->death_save_successes << "/3)";
    if (updated->death_save_successes >= 3) {
      text << ", стабилен на " << stable_seconds / kSecondsPerHour << " ч";
    }
  } else {
    text << " — провал (" << updated->death_save_failures << "/3)";
    if (updated->death_save_failures >= 3) {
      text << ", персонаж погиб";
    }
  }
  events.emplace_back(LogLineAdded{
      .line = LogLine{.author = std::string(author),
                      .text = text.str(),
                      .timestamp = CurrentTimeSeconds()},
  });
  return updated;
}

auto GameServer::RollStableSeconds() -> int {
  std::uniform_int_distribution<int> d4(1, 4);
  return d4(rng_) * kSecondsPerHour;
}

auto GameServer::HandleStabilize(const Stabilize& command, bool can_control_all)
    -> CommandOutcome {
  if (!can_control_all) {
    return CommandOutcome{.reject_reason = "стабилизирует только DM"};
  }
  const Token* token = FindToken(command.id);
  if (token == nullptr) {
    return CommandOutcome{.reject_reason = "токен не найден"};
  }
  if (token->hp > 0) {
    return CommandOutcome{.reject_reason = "стабилизировать можно при 0 HP"};
  }
  if (HasCondition(*token, kDead)) {
    return CommandOutcome{.reject_reason = "персонаж уже мёртв"};
  }
  if (HasCondition(*token, kStable) || token->death_save_successes >= 3) {
    return CommandOutcome{.reject_reason = "персонаж уже стабилен"};
  }
  // Навыки появятся отдельным шагом: пока Медицина — это d20 без бонуса.
  std::uniform_int_distribution<int> d20(1, 20);
  const int roll = d20(rng_);
  const int stable_seconds = RollStableSeconds();
  std::ostringstream text;
  text << token->name << ": Медицина (КС 10) " << roll;
  CommandOutcome outcome;
  outcome.accepted = true;
  if (roll >= 10) {
    const std::optional<Token> updated =
        state_.StabilizeToken(command.id, stable_seconds);
    if (!updated.has_value()) {
      return CommandOutcome{.reject_reason = "не удалось стабилизировать"};
    }
    text << " — успех, стабилен на " << stable_seconds / kSecondsPerHour
         << " ч";
    outcome.events.emplace_back(TokenUpdated{.token = *updated});
  } else {
    text << " — провал";
  }
  outcome.events.emplace_back(LogLineAdded{
      .line = LogLine{.author = "DM",
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
  // Выбор даётся один раз: после того как DM снял владельца, персонажа
  // назначает только DM, а не прежний игрок повторным «забрать».
  const bool already_owned = SamePlayerName(token->owner, actor);
  if (!already_owned && claimed_players_.contains(PlayerNameKey(actor))) {
    return CommandOutcome{
        .reject_reason = "персонажа выбирают один раз, дальше назначает DM"};
  }
  const std::optional<Token> updated = state_.SetTokenOwner(command.id, actor);
  if (!updated.has_value()) {
    return CommandOutcome{.reject_reason = "не удалось забрать персонажа"};
  }
  claimed_players_.insert(PlayerNameKey(actor));
  RecomputeFog();
  CommandOutcome outcome;
  outcome.accepted = true;
  outcome.events.emplace_back(TokenUpdated{.token = *updated});
  return outcome;
}

auto GameServer::HandleReleaseToken(const ReleaseToken& command,
                                    bool can_control_all) -> CommandOutcome {
  if (!can_control_all) {
    return CommandOutcome{.reject_reason = "персонажа освобождает только DM"};
  }
  const Token* token = FindToken(command.id);
  if (token == nullptr) {
    return CommandOutcome{.reject_reason = "токен не найден"};
  }
  // Монстры и NPC всегда принадлежат DM: снять владельца нельзя, иначе
  // токен станет свободным и его начнут двигать игроки.
  if (token->kind != TokenKind::kPlayer) {
    return CommandOutcome{.reject_reason = "монстр всегда принадлежит DM"};
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
  const Token* token = FindToken(command.id);
  if (token == nullptr) {
    return CommandOutcome{.reject_reason = "токен не найден"};
  }
  // Монстра нельзя освободить или передать игроку: пока ими владеет только DM.
  if (token->kind != TokenKind::kPlayer &&
      !SamePlayerName(command.owner, kDmOwner)) {
    return CommandOutcome{.reject_reason = "монстр всегда принадлежит DM"};
  }
  const std::optional<Token> updated =
      state_.SetTokenOwner(command.id, command.owner);
  if (!updated.has_value()) {
    return CommandOutcome{.reject_reason = "токен не найден"};
  }
  // Назначенный DM игрок тоже считается выбравшим: снятие владельца позже
  // не даёт ему забрать персонажа самому.
  if (updated->kind == TokenKind::kPlayer && !command.owner.empty()) {
    claimed_players_.insert(PlayerNameKey(command.owner));
  }
  RecomputeFog();
  CommandOutcome outcome;
  outcome.accepted = true;
  outcome.events.emplace_back(TokenUpdated{.token = *updated});
  return outcome;
}

auto GameServer::HandleRenameToken(const RenameToken& command,
                                   bool can_control_all) -> CommandOutcome {
  if (!can_control_all) {
    return CommandOutcome{.reject_reason = "переименовывает токен только DM"};
  }
  const Token* token = FindToken(command.id);
  if (token == nullptr) {
    return CommandOutcome{.reject_reason = "токен не найден"};
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
  if (token->auto_hp) {
    return CommandOutcome{.reject_reason =
                              "максимум HP считается по костям (лист L)"};
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
                                     bool can_control_all) -> CommandOutcome {
  if (!can_control_all) {
    return CommandOutcome{.reject_reason = "цвет токена меняет только DM"};
  }
  const Token* token = FindToken(command.id);
  if (token == nullptr) {
    return CommandOutcome{.reject_reason = "токен не найден"};
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

auto GameServer::HandleSetTokenLight(const SetTokenLight& command,
                                     const std::string& actor,
                                     bool can_control_all) -> CommandOutcome {
  const Token* token = FindToken(command.id);
  if (token == nullptr) {
    return CommandOutcome{.reject_reason = "токен не найден"};
  }
  if (!CanControl(*token, actor, can_control_all)) {
    return CommandOutcome{.reject_reason = ControlRejectReason(*token)};
  }
  if (command.bright_cells < 0 || command.bright_cells > kMaxLightCells ||
      command.dim_cells < command.bright_cells ||
      command.dim_cells > kMaxLightCells) {
    return CommandOutcome{.reject_reason = "некорректный свет"};
  }
  const std::optional<Token> updated =
      state_.SetTokenLight(command.id, command.bright_cells, command.dim_cells);
  if (!updated.has_value()) {
    return CommandOutcome{.reject_reason = "не удалось изменить свет"};
  }
  // Свет факела меняет и поле света, и туман всех игроков.
  RecomputeFog();
  CommandOutcome outcome;
  outcome.accepted = true;
  outcome.events.emplace_back(TokenUpdated{.token = *updated});
  outcome.events.emplace_back(LogLineAdded{
      .line =
          LogLine{.author = actor,
                  .text = command.bright_cells > 0
                              ? token->name + ": свет " +
                                    std::to_string(command.bright_cells) + "/" +
                                    std::to_string(command.dim_cells) +
                                    " клеток"
                              : token->name + ": свет выключен",
                  .timestamp = CurrentTimeSeconds()},
  });
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
      command.initiative_bonus > kMaxBonus ||
      command.skill_bonus < -kMaxBonus || command.skill_bonus > kMaxBonus) {
    return CommandOutcome{.reject_reason = "некорректный бонус"};
  }
  if (!IsValidSkillMask(command.skill_proficiency)) {
    return CommandOutcome{.reject_reason = "неизвестный навык во владениях"};
  }
  if (command.damage.size() > kMaxDamageExpression) {
    return CommandOutcome{.reject_reason = "слишком длинное выражение урона"};
  }
  if (!command.damage.empty()) {
    DiceExpression damage;
    DamageType damage_type = DamageType::kNone;
    if (!ParseDamageProfile(command.damage, damage, damage_type)) {
      return CommandOutcome{.reject_reason = "некорректное выражение урона"};
    }
  }
  if (command.attack_range < 0 || command.attack_range > kMaxSpeed) {
    return CommandOutcome{.reject_reason = "некорректная дистанция атаки"};
  }
  if (!IsValidDamageTypeBit(command.resist) ||
      !IsValidDamageTypeBit(command.vulnerable) ||
      !IsValidDamageTypeBit(command.immune)) {
    return CommandOutcome{.reject_reason = "некорректная защита от урона"};
  }
  if (command.temp_hp < 0 || command.temp_hp > kMaxTempHp) {
    return CommandOutcome{.reject_reason = "некорректные временные HP"};
  }
  if (command.max_hp <= 0 || command.max_hp > kMaxHp) {
    return CommandOutcome{.reject_reason = "некорректный максимум HP"};
  }
  if (command.hit_dice_count < 0 || command.hit_dice_count > 1000) {
    return CommandOutcome{.reject_reason = "некорректное число костей хитов"};
  }
  if (!IsValidHitDie(command.hit_die)) {
    return CommandOutcome{.reject_reason = "некорректная кость хитов"};
  }
  if (command.con_modifier < -kMaxConModifier ||
      command.con_modifier > kMaxConModifier) {
    return CommandOutcome{.reject_reason =
                              "некорректный модификатор Телосложения"};
  }
  if (command.swim_speed < 0 || command.swim_speed > kMaxSpeed ||
      command.climb_speed < 0 || command.climb_speed > kMaxSpeed) {
    return CommandOutcome{.reject_reason =
                              "некорректная скорость плавания или лазания"};
  }
  if (command.darkvision_cells < 0 ||
      command.darkvision_cells > kMaxLightCells) {
    return CommandOutcome{.reject_reason = "некорректное тёмное зрение"};
  }
  const Token before = *token;
  const std::optional<Token> updated = state_.UpdateTokenSheet(
      command.id, command.ac, command.speed, command.notes,
      command.attack_bonus, command.save_bonus, command.initiative_bonus,
      command.damage, command.temp_hp, command.hit_die, command.con_modifier,
      command.attack_range, command.resist, command.vulnerable, command.immune,
      command.swim_speed, command.climb_speed, command.darkvision_cells,
      command.max_hp, command.auto_hp, command.hit_dice_count,
      command.skill_bonus, command.skill_proficiency);
  if (!updated.has_value()) {
    return CommandOutcome{.reject_reason = "не удалось обновить лист"};
  }
  // Тёмное зрение из листа расширяет туман владельца.
  RecomputeFog();
  CommandOutcome outcome;
  outcome.accepted = true;
  outcome.events.emplace_back(TokenUpdated{.token = *updated});
  std::vector<std::string> changes;
  AppendIntChange(changes, "КД", before.ac, command.ac);
  AppendIntChange(changes, "скорость", before.speed, command.speed);
  AppendIntChange(changes, "скорость плавания", before.swim_speed,
                  command.swim_speed);
  AppendIntChange(changes, "скорость лазания", before.climb_speed,
                  command.climb_speed);
  // Лист и интерфейс показывают тёмное зрение в метрах, хотя внутри оно
  // хранится в клетках: лог не должен путать игрока.
  AppendIntChange(changes, "тёмное зрение",
                  LightMetersFromCells(before.darkvision_cells),
                  LightMetersFromCells(command.darkvision_cells));
  AppendIntChange(changes, "бонус атаки", before.attack_bonus,
                  command.attack_bonus);
  AppendIntChange(changes, "бонус спасброска", before.save_bonus,
                  command.save_bonus);
  AppendIntChange(changes, "бонус инициативы", before.initiative_bonus,
                  command.initiative_bonus);
  AppendIntChange(changes, "бонус проверок", before.skill_bonus,
                  command.skill_bonus);
  if (before.skill_proficiency != command.skill_proficiency) {
    changes.emplace_back("владения навыками: " +
                         FormatSkillMask(command.skill_proficiency));
  }
  AppendIntChange(changes, "временные HP", before.temp_hp, updated->temp_hp);
  AppendIntChange(changes, "макс. HP", before.max_hp, updated->max_hp);
  AppendIntChange(changes, "костей хитов", before.hit_dice_count,
                  command.hit_dice_count);
  if (before.auto_hp != command.auto_hp) {
    changes.emplace_back(command.auto_hp ? "авто HP включено"
                                         : "авто HP выключено");
  }
  AppendIntChange(changes, "мод. Телосложения", before.con_modifier,
                  command.con_modifier);
  AppendIntChange(changes, "дистанция атаки", before.attack_range,
                  command.attack_range);
  if (before.hit_die != command.hit_die) {
    changes.emplace_back("кость хитов d" + std::to_string(command.hit_die));
  }
  if (before.resist != updated->resist) {
    const std::string list = FormatDamageTypeList(updated->resist);
    changes.emplace_back("сопротивления " +
                         (list.empty() ? std::string("нет") : list));
  }
  if (before.vulnerable != updated->vulnerable) {
    const std::string list = FormatDamageTypeList(updated->vulnerable);
    changes.emplace_back("уязвимости " +
                         (list.empty() ? std::string("нет") : list));
  }
  if (before.immune != updated->immune) {
    const std::string list = FormatDamageTypeList(updated->immune);
    changes.emplace_back("иммунитеты " +
                         (list.empty() ? std::string("нет") : list));
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
    // Источник состояния должен быть другим существом на столе.
    if (condition.source_id != 0 &&
        (condition.source_id == command.id ||
         FindToken(condition.source_id) == nullptr)) {
      return CommandOutcome{.reject_reason = "некорректный источник состояния"};
    }
  }
  const auto exhaustion_level = std::ranges::count_if(
      command.conditions, [](const Condition& condition) -> bool {
        return condition.name == kExhaustion;
      });
  if (exhaustion_level > kMaxExhaustionLevel) {
    return CommandOutcome{.reject_reason = "уровень истощения не выше 6"};
  }
  const Token before = *token;
  std::optional<Token> updated =
      state_.UpdateTokenCombat(command.id, command.conditions);
  if (!updated.has_value()) {
    return CommandOutcome{.reject_reason = "не удалось обновить состояния"};
  }
  // Недееспособность и смерть рвут концентрацию без спасброска.
  bool concentration_broken = false;
  if (HasCondition(*updated, kConcentration) && IsIncapacitated(*updated)) {
    if (const std::optional<Token> without =
            state_.RemoveTokenCondition(command.id, kConcentration)) {
      updated = *without;
      concentration_broken = true;
    }
  }
  // Состояния меняют зрение: слепота, «без сознания» и смерть гасят обзор,
  // поэтому туман пересчитывается сразу, а не к следующему действию.
  RecomputeFog();
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
    if (condition.name == kExhaustion) {
      continue;
    }
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
      if (const Token* source = FindToken(condition.source_id);
          condition.source_id != 0 && source != nullptr) {
        entry += ", источник: " + source->name;
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
    if (before_condition->source_id != condition.source_id) {
      const Token* source = FindToken(condition.source_id);
      changes.push_back(
          condition.name + ": источник " +
          (source == nullptr ? std::string("снят") : source->name));
    }
  }
  for (const Condition& condition : before.conditions) {
    if (condition.name == kExhaustion) {
      continue;
    }
    if (!has_condition(command.conditions, condition.name)) {
      changes.emplace_back("−" + condition.name);
    }
  }
  // Истощение — несколько записей с одним именем, поэтому сравнивать его по
  // имени бесполезно: повышение уровня писалось бы как повтор уже имеющейся
  // записи. Уровень считаем отдельно, чтобы каждый шаг попадал в лог.
  const int exhaustion_before = ExhaustionLevel(before);
  const int exhaustion_after = ExhaustionLevel(*updated);
  if (exhaustion_after != exhaustion_before) {
    changes.push_back("истощение " + std::to_string(exhaustion_after) + "/" +
                      std::to_string(kMaxExhaustionLevel));
  }
  if (concentration_broken) {
    changes.emplace_back("−" + std::string(kConcentration) +
                         " (недееспособен)");
  }
  if (!changes.empty()) {
    outcome.events.emplace_back(LogLineAdded{
        .line = LogLine{.author = "DM",
                        .text = "Состояния " + before.name + ": " +
                                JoinChanges(changes),
                        .timestamp = CurrentTimeSeconds()},
    });
  }
  // Шестой уровень истощения убивает: состояние выставили через панель, а
  // смерть ставит GameState. Сообщаем об этом отдельной строкой.
  if (!HasCondition(before, kDead) && HasCondition(*updated, kDead)) {
    outcome.events.emplace_back(LogLineAdded{
        .line = LogLine{.author = "DM",
                        .text = before.name + ": истощение 6 — смерть",
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

auto GameServer::IsPartyVisible(Cell cell) const -> bool {
  return party_fog_.IsVisible(cell) || state_.IsRevealed(cell);
}

auto GameServer::DefaultParticipants() const -> std::vector<TokenId> {
  const bool has_players =
      std::ranges::any_of(state_.Tokens(), [](const Token& token) -> bool {
        return token.kind == TokenKind::kPlayer;
      });
  std::vector<TokenId> participants;
  participants.reserve(state_.Tokens().size());
  for (const Token& token : state_.Tokens()) {
    if (token.kind == TokenKind::kPlayer) {
      // Персонажи в бою всегда: они — сторона, которая бьётся.
      participants.push_back(token.id);
      continue;
    }
    // Без персонажей решать не по чему: бой начинает весь стол.
    if (!has_players) {
      participants.push_back(token.id);
      continue;
    }
    // Враждовать или нет, в токене не хранится (упрощение): замеченный
    // столом не-персонаж вступает в бой, лежащий на нуле — нет.
    if (token.hp > 0 && IsPartyVisible(token.cell)) {
      participants.push_back(token.id);
    }
  }
  return participants;
}

auto GameServer::RollInitiative(TokenId id) -> int {
  int value = 0;
  if (const std::optional<Token> token = state_.TokenCopy(id)) {
    std::uniform_int_distribution<int> d20(1, 20);
    const int bonus = token->initiative_bonus - ExhaustionPenalty(*token);
    value = d20(rng_) + bonus;
    // 2024: невидимый бросает с преимуществом, недееспособный и застигнутый
    // врасплох — с помехой. Преимущество и помеха гасятся, поэтому второй
    // бросок нужен только при одной поправке. Внезапность тратится сразу:
    // отдельного раунда неожиданности в 2024 нет.
    const AttackRollModes modes = InitiativeRollModesFor(*token);
    if (modes.Net() > 0) {
      value = std::max(value, d20(rng_) + bonus);
    } else if (modes.Net() < 0) {
      value = std::min(value, d20(rng_) + bonus);
    }
    if (IsSurprised(*token)) {
      (void)state_.RemoveTokenCondition(id, kSurprised);
    }
  }
  (void)state_.SetInitiative(id, value);
  return value;
}

auto GameServer::JoinEncounterIfNeeded(TokenId id, std::vector<Event>& events)
    -> bool {
  if (!state_.EncounterState().active || state_.IsEncounterParticipant(id)) {
    return false;
  }
  const std::optional<Token> token = state_.TokenCopy(id);
  if (!token.has_value() || token->hp <= 0) {
    return false;
  }
  const int initiative = RollInitiative(id);
  if (!state_.JoinEncounter(id)) {
    return false;
  }
  if (const std::optional<Token> joined = state_.TokenCopy(id)) {
    events.emplace_back(TokenUpdated{.token = *joined});
  }
  events.emplace_back(EncounterUpdated{.encounter = state_.EncounterState()});
  events.emplace_back(LogLineAdded{
      .line = LogLine{.author = "DM",
                      .text = token->name + " вступает в бой (инициатива " +
                              std::to_string(initiative) + ")",
                      .timestamp = CurrentTimeSeconds()},
  });
  return true;
}

auto GameServer::HandleStartEncounter(bool can_control_all) -> CommandOutcome {
  if (!can_control_all) {
    return CommandOutcome{.reject_reason = "бой начинает только DM"};
  }
  if (state_.EncounterState().active) {
    return CommandOutcome{.reject_reason = "бой уже идёт"};
  }
  const std::vector<TokenId> participants = DefaultParticipants();
  if (participants.empty()) {
    return CommandOutcome{.reject_reason = "некого включать в бой"};
  }
  const std::vector<TokenId> spent = TokensWithTurnState(state_);
  // Инициатива бросается только за участников: спящий за стеной или ещё не
  // замеченный монстр в порядок не попадает и вступит позже, когда его
  // заметят или ударят. Это же значение потом можно поправить через [ и ].
  std::uniform_int_distribution<int> d20(1, 20);
  std::vector<std::pair<int, std::string>> rolls;
  rolls.reserve(participants.size());
  std::vector<TokenId> changed = spent;
  const auto remember = [&changed](TokenId id) -> void {
    if (std::ranges::find(changed, id) == changed.end()) {
      changed.push_back(id);
    }
  };
  for (const TokenId id : participants) {
    const std::optional<Token> token = state_.TokenCopy(id);
    if (!token.has_value()) {
      continue;
    }
    const int bonus = token->initiative_bonus - ExhaustionPenalty(*token);
    const bool surprised = IsSurprised(*token);
    int value = d20(rng_) + bonus;
    if (surprised) {
      value = std::min(value, d20(rng_) + bonus);
    }
    const std::string label =
        surprised ? token->name + " (застигнут врасплох)" : token->name;
    rolls.emplace_back(value, label);
    if (state_.SetInitiative(id, value).has_value()) {
      remember(id);
    }
    if (surprised && state_.RemoveTokenCondition(id, kSurprised).has_value()) {
      remember(id);
    }
  }
  if (!state_.StartEncounter(participants)) {
    return CommandOutcome{.reject_reason = "некого включать в бой"};
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
  // Умирающий персонаж бросает спасбросок в начале своего хода.
  (void)RollDeathSave(state_.ActiveTokenId(), "DM", true, outcome.events);
  for (const TokenId id : changed) {
    if (std::optional<Token> token = state_.TokenCopy(id)) {
      outcome.events.emplace_back(TokenUpdated{.token = std::move(*token)});
    }
  }
  TryCurrent(state_.ActiveTokenId(), "DM", outcome.events);
  TriggerHazards(state_.ActiveTokenId(), kHazardTurnStart, "DM",
                 outcome.events);
  return outcome;
}

auto GameServer::HandleSetEncounterComposition(
    const SetEncounterComposition& command, bool can_control_all)
    -> CommandOutcome {
  if (!can_control_all) {
    return CommandOutcome{.reject_reason = "состав боя правит только DM"};
  }
  if (!state_.EncounterState().active) {
    return CommandOutcome{.reject_reason = "бой не начат"};
  }
  const Token* token = FindToken(command.id);
  if (token == nullptr) {
    return CommandOutcome{.reject_reason = "токен не найден"};
  }
  const std::string name = token->name;
  if (command.joined) {
    if (state_.IsEncounterParticipant(command.id)) {
      return CommandOutcome{.accepted = true};
    }
    CommandOutcome outcome;
    outcome.accepted = true;
    if (!JoinEncounterIfNeeded(command.id, outcome.events)) {
      return CommandOutcome{.reject_reason = "лежащий не вступает в бой"};
    }
    return outcome;
  }
  if (!state_.LeaveEncounter(command.id)) {
    return CommandOutcome{.accepted = true};
  }
  CommandOutcome outcome;
  outcome.accepted = true;
  outcome.events.emplace_back(
      EncounterUpdated{.encounter = state_.EncounterState()});
  // Ход перешёл к следующему: у него сброшены движение, действие и реакция.
  if (const std::optional<Token> active =
          state_.TokenCopy(state_.ActiveTokenId())) {
    outcome.events.emplace_back(TokenUpdated{.token = *active});
  }
  outcome.events.emplace_back(LogLineAdded{
      .line = LogLine{.author = "DM",
                      .text = name + " выведен из боя",
                      .timestamp = CurrentTimeSeconds()},
  });
  return outcome;
}

auto GameServer::HandleEndEncounter(bool can_control_all) -> CommandOutcome {
  if (!can_control_all) {
    return CommandOutcome{.reject_reason = "бой завершает только DM"};
  }
  if (!state_.EncounterState().active) {
    return CommandOutcome{.reject_reason = "нет активного боя"};
  }
  const std::vector<TokenId> spent = TokensWithTurnState(state_);
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
      const int save_bonus = ending->save_bonus - ExhaustionPenalty(*ending);
      const int roll = d20(rng_) + save_bonus;
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

  const std::vector<TokenId> spent = TokensWithTurnState(state_);
  TriggerHazards(ending_id, kHazardTurnEnd, "DM", outcome.events);
  if (!state_.NextTurn()) {
    return CommandOutcome{.reject_reason = "нет активного боя"};
  }
  for (const TokenId id : spent) {
    remember(id);
  }
  // У нового активного токена время хода уже срезало его состояния.
  remember(state_.ActiveTokenId());
  if (const std::optional<Token> active =
          state_.TokenCopy(state_.ActiveTokenId())) {
    outcome.events.emplace_back(LogLineAdded{
        .line =
            LogLine{.author = "DM",
                    .text = "Ход: " + active->name + " (раунд " +
                            std::to_string(state_.EncounterState().round) + ")",
                    .timestamp = CurrentTimeSeconds()},
    });
  }
  // Начало хода умирающего: спасбросок от смерти бросает сервер.
  (void)RollDeathSave(state_.ActiveTokenId(), "DM", true, outcome.events);
  for (const TokenId id : changed) {
    if (std::optional<Token> token = state_.TokenCopy(id)) {
      outcome.events.emplace_back(TokenUpdated{.token = std::move(*token)});
    }
  }
  outcome.events.emplace_back(
      EncounterUpdated{.encounter = state_.EncounterState()});
  TryCurrent(state_.ActiveTokenId(), "DM", outcome.events);
  TriggerHazards(state_.ActiveTokenId(), kHazardTurnStart, "DM",
                 outcome.events);
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
  const std::string key = PlayerNameKey(actor);
  const auto it = fogs_.find(key);
  if (it == fogs_.end()) {
    return nullptr;
  }
  // Туман прошлой партии не должен закрывать общий обзор: пока у игрока нет
  // персонажа, он видит то же, что и вся партия.
  if (!character_owners_.contains(key)) {
    return nullptr;
  }
  return &it->second;
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
    return !IsTokenVisibleTo(actor, token);
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
      .light = std::vector<std::uint8_t>(
          cells, static_cast<std::uint8_t>(LightLevel::kBright)),
  };
}

auto GameServer::IsTokenVisibleTo(std::string_view actor,
                                  const Token& token) const -> bool {
  // Сильное затемнение (тьма, дым) прячет существо, даже если клетку видно:
  // позиция неизвестна, пока оно само не выдаст себя.
  if (IsVisibleTo(actor, token.cell) &&
      state_.Map().ObscurementAt(token.cell) != Obscurement::kHeavy) {
    // Скрытного замечает пассивное Восприятие персонажей игрока.
    return !token.hidden || NoticesHidden(actor, token);
  }
  return false;
}

auto GameServer::NoticesHidden(std::string_view actor, const Token& token) const
    -> bool {
  // Свой токен игрок видит всегда, а DM — весь стол.
  if (SamePlayerName(actor, token.owner)) {
    return true;
  }
  int best = 0;
  bool has_character = false;
  // Сначала персонажи самого игрока; без них он смотрит общим обзором партии,
  // поэтому скрытного замечает любой персонаж партии.
  for (const Token& observer : state_.Tokens()) {
    if (observer.kind != TokenKind::kPlayer ||
        !SamePlayerName(observer.owner, actor)) {
      continue;
    }
    has_character = true;
    best = std::max(best, PassivePerception(observer));
  }
  if (!has_character) {
    for (const Token& observer : state_.Tokens()) {
      if (observer.kind == TokenKind::kPlayer) {
        best = std::max(best, PassivePerception(observer));
      }
    }
  }
  return best >= token.stealth_total;
}

auto GameServer::PassivePerception(const Token& observer) const -> int {
  // Тусклый свет даёт помеху на зрительное Восприятие: пассивное значение −5.
  const bool dim = light_.At(observer.cell) == LightLevel::kDim;
  return PassiveSkillScore(observer, Skill::kPerception, dim);
}

auto GameServer::EventVisibleTo(const Event& event,
                                std::string_view actor) const -> bool {
  return std::visit(
      [this, actor](const auto& ev) -> bool {
        using T = std::decay_t<decltype(ev)>;
        if constexpr (std::is_same_v<T, TokenMoved>) {
          return IsVisibleTo(actor, ev.to) &&
                 state_.Map().ObscurementAt(ev.to) != Obscurement::kHeavy;
        } else if constexpr (std::is_same_v<T, TokenAdded> ||
                             std::is_same_v<T, TokenUpdated>) {
          return IsTokenVisibleTo(actor, ev.token);
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
    if (IsTokenVisibleTo(actor, token)) {
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
      sent.fog.light.size() == cells && fog.explored.size() == cells &&
      fog.light.size() == cells;
  const int width = std::max(fog.width, 1);
  const auto fog_cell = [&fog, width](std::size_t index) -> FogCell {
    return FogCell{
        .cell =
            Cell{
                .x = static_cast<int>(index % static_cast<std::size_t>(width)),
                .y = static_cast<int>(index / static_cast<std::size_t>(width))},
        .visible = fog.visible[index] != 0,
        .explored = fog.explored[index] != 0,
        .light = fog.light[index],
    };
  };
  FogChanged changed;
  for (std::size_t i = 0; i < cells; ++i) {
    // Другая карта: точечная правка невозможна, отдаём туман целиком.
    if (same_map && sent.fog.visible[i] == fog.visible[i] &&
        sent.fog.explored[i] == fog.explored[i] &&
        sent.fog.light[i] == fog.light[i]) {
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
    if (IsTokenVisibleTo(actor, token)) {
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
  // Свет сцены считается один раз: свойства клеток плюс свет всех токенов.
  // Он же нужен атакам: дым и тьма закрывают взгляд (core/conditions).
  light_ = ComputeLightField(state_.Map(), TokenLightSources(state_.Tokens()));
  auto observer = [](const Token& token) -> Observer {
    return Observer{.cell = token.cell,
                    .darkvision_cells = token.darkvision_cells,
                    .blinded = HasCondition(token, kBlinded)};
  };
  // Слепой, лежачий без сознания и мёртвый персонаж обзора не даёт.
  auto sees = [](const Token& token) -> bool {
    return !HasCondition(token, kBlinded) &&
           !HasCondition(token, kUnconscious) && !HasCondition(token, kDead);
  };

  std::map<std::string, std::vector<Observer>, std::less<>> observers;
  std::vector<Observer> party_observers;
  character_owners_.clear();
  for (const Token& token : state_.Tokens()) {
    if (token.kind != TokenKind::kPlayer) {
      continue;
    }
    // Владелец персонажа остаётся владельцем и тогда, когда персонаж ослеплён,
    // без сознания или мёртв: такой игрок смотрит в свой пустой туман, а не
    // получает партийный обзор. Взятые DM-ом персонажи тумана игрока не
    // создают: DM видит весь стол, а партийный обзор их видит как обычных.
    const bool player_owned =
        !token.owner.empty() && !SamePlayerName(token.owner, kDmOwner);
    if (player_owned) {
      character_owners_.insert(PlayerNameKey(token.owner));
    }
    if (!sees(token)) {
      continue;
    }
    party_observers.push_back(observer(token));
    if (player_owned) {
      observers[PlayerNameKey(token.owner)].push_back(observer(token));
    }
  }

  const std::vector<Cell>& revealed = state_.Revealed();
  party_fog_.Recompute(state_.Map(), party_observers, light_, sight_radius_);
  party_fog_.Reveal(revealed);
  // Игрок без персонажа больше не видит клетку, но помнит исследованное.
  for (auto& [owner, fog] : fogs_) {
    const auto it = observers.find(owner);
    fog.Recompute(state_.Map(),
                  it == observers.end() ? std::vector<Observer>{} : it->second,
                  light_, sight_radius_);
    fog.Reveal(revealed);
  }
  for (const std::string& owner : character_owners_) {
    if (fogs_.contains(owner)) {
      continue;
    }
    FogOfWar fog(state_.Map().Width(), state_.Map().Height());
    const auto it = observers.find(owner);
    fog.Recompute(state_.Map(),
                  it == observers.end() ? std::vector<Observer>{} : it->second,
                  light_, sight_radius_);
    fog.Reveal(revealed);
    fogs_.emplace(owner, std::move(fog));
  }
}

}  // namespace dnd
