#include "core/game_state.h"

#include <algorithm>
#include <type_traits>
#include <utility>

namespace dnd {

GameState::GameState(GridMap map) : map_(std::move(map)) {}

GameState::GameState(GameSnapshot snapshot)
    : map_(std::move(snapshot.map)),
      tokens_(std::move(snapshot.tokens)),
      next_token_id_(snapshot.next_token_id),
      encounter_(std::move(snapshot.encounter)),
      aoe_(snapshot.aoe) {}

auto GameState::Snapshot() const -> GameSnapshot {
  return GameSnapshot{
      .map = map_,
      .tokens = tokens_,
      .next_token_id = next_token_id_,
      .encounter = encounter_,
      .aoe = aoe_,
  };
}

auto GameState::TokenCopy(TokenId id) const -> std::optional<Token> {
  const Token* token = FindToken(id);
  if (token == nullptr) {
    return std::nullopt;
  }
  return *token;
}

auto GameState::AddToken(std::string name, Cell cell, std::string owner,
                         TokenKind kind, int max_hp, std::uint32_t color,
                         int ac, int speed, std::string notes, int attack_bonus,
                         int save_bonus, std::string damage) -> TokenId {
  const TokenId active = ActiveTokenId();
  const TokenId id = next_token_id_++;
  const int resolved_max_hp = std::max(1, max_hp);
  tokens_.push_back(Token{.id = id,
                          .name = std::move(name),
                          .owner = std::move(owner),
                          .hp = resolved_max_hp,
                          .max_hp = resolved_max_hp,
                          .ac = ac,
                          .speed = speed,
                          .notes = std::move(notes),
                          .attack_bonus = attack_bonus,
                          .save_bonus = save_bonus,
                          .damage = std::move(damage),
                          .kind = kind,
                          .color = color,
                          .cell = cell});
  if (encounter_.active) {
    RebuildOrder(active);
  }
  return id;
}

auto GameState::FindToken(TokenId id) -> Token* {
  for (auto& token : tokens_) {
    if (token.id == id) {
      return &token;
    }
  }
  return nullptr;
}

auto GameState::FindToken(TokenId id) const -> const Token* {
  for (const auto& token : tokens_) {
    if (token.id == id) {
      return &token;
    }
  }
  return nullptr;
}

auto GameState::RemoveToken(TokenId id) -> bool {
  const auto it = std::ranges::find_if(
      tokens_, [id](const Token& token) -> bool { return token.id == id; });
  if (it == tokens_.end()) {
    return false;
  }
  tokens_.erase(it);
  if (encounter_.active) {
    std::erase(encounter_.order, id);
    if (encounter_.order.empty()) {
      encounter_ = Encounter{};
    } else if (encounter_.turn_index >= encounter_.order.size()) {
      encounter_.turn_index = 0;
    }
  }
  return true;
}

auto GameState::SetTokenHp(TokenId id, int hp) -> std::optional<Token> {
  Token* token = FindToken(id);
  if (token == nullptr) {
    return std::nullopt;
  }
  token->hp = std::clamp(hp, 0, token->max_hp);
  return *token;
}

auto GameState::SetInitiative(TokenId id, int value) -> std::optional<Token> {
  Token* token = FindToken(id);
  if (token == nullptr) {
    return std::nullopt;
  }
  const TokenId active = ActiveTokenId();
  token->initiative = value;
  if (encounter_.active) {
    RebuildOrder(active);
  }
  return *token;
}

auto GameState::SetTokenOwner(TokenId id, std::string owner)
    -> std::optional<Token> {
  Token* token = FindToken(id);
  if (token == nullptr) {
    return std::nullopt;
  }
  token->owner = std::move(owner);
  return *token;
}

auto GameState::RenameToken(TokenId id, std::string name)
    -> std::optional<Token> {
  Token* token = FindToken(id);
  if (token == nullptr) {
    return std::nullopt;
  }
  token->name = std::move(name);
  return *token;
}

auto GameState::SetTokenMaxHp(TokenId id, int max_hp) -> std::optional<Token> {
  Token* token = FindToken(id);
  if (token == nullptr) {
    return std::nullopt;
  }
  token->max_hp = std::max(1, max_hp);
  token->hp = std::clamp(token->hp, 0, token->max_hp);
  return *token;
}

auto GameState::SetTokenColor(TokenId id, std::uint32_t color)
    -> std::optional<Token> {
  Token* token = FindToken(id);
  if (token == nullptr) {
    return std::nullopt;
  }
  token->color = color & 0xFFFFFFU;
  return *token;
}

auto GameState::UpdateTokenSheet(TokenId id, int ac, int speed,
                                 std::string notes, int attack_bonus,
                                 int save_bonus, std::string damage,
                                 int temp_hp) -> std::optional<Token> {
  Token* token = FindToken(id);
  if (token == nullptr) {
    return std::nullopt;
  }
  token->ac = ac;
  token->speed = speed;
  token->notes = std::move(notes);
  token->attack_bonus = attack_bonus;
  token->save_bonus = save_bonus;
  token->damage = std::move(damage);
  token->temp_hp = temp_hp;
  return *token;
}

auto GameState::UpdateTokenCombat(TokenId id,
                                  std::vector<std::string> conditions)
    -> std::optional<Token> {
  Token* token = FindToken(id);
  if (token == nullptr) {
    return std::nullopt;
  }
  token->conditions = std::move(conditions);
  return *token;
}

auto GameState::ActiveTokenId() const -> TokenId {
  if (!encounter_.active || encounter_.turn_index >= encounter_.order.size()) {
    return 0;
  }
  return encounter_.order[encounter_.turn_index];
}

auto GameState::StartEncounter() -> bool {
  if (tokens_.empty()) {
    return false;
  }
  encounter_ = Encounter{};
  encounter_.active = true;
  encounter_.round = 1;
  RebuildOrder(0);
  return true;
}

auto GameState::EndEncounter() -> void { encounter_ = Encounter{}; }

auto GameState::SetAoe(AoeTemplate aoe) -> bool {
  if (!IsValidAoe(aoe) ||
      (aoe.shape != AoeShape::kNone && !map_.Contains(aoe.origin))) {
    return false;
  }
  aoe_ = aoe;
  return true;
}

auto GameState::NextTurn() -> bool {
  if (!encounter_.active || encounter_.order.empty()) {
    return false;
  }
  ++encounter_.turn_index;
  if (encounter_.turn_index >= encounter_.order.size()) {
    encounter_.turn_index = 0;
    ++encounter_.round;
  }
  return true;
}

auto GameState::RebuildOrder(TokenId keep) -> void {
  std::vector<TokenId> order;
  order.reserve(tokens_.size());
  for (const Token& token : tokens_) {
    order.push_back(token.id);
  }
  std::ranges::sort(order, [this](TokenId left, TokenId right) -> bool {
    const Token* left_token = FindToken(left);
    const Token* right_token = FindToken(right);
    if (left_token == nullptr || right_token == nullptr) {
      return left < right;
    }
    if (left_token->initiative != right_token->initiative) {
      return left_token->initiative > right_token->initiative;
    }
    if (left_token->name != right_token->name) {
      return left_token->name < right_token->name;
    }
    return left < right;
  });
  encounter_.order = std::move(order);
  if (encounter_.order.empty()) {
    encounter_.active = false;
    encounter_.round = 0;
    encounter_.turn_index = 0;
    return;
  }
  const auto it = std::ranges::find(encounter_.order, keep);
  encounter_.turn_index =
      it != encounter_.order.end()
          ? static_cast<std::size_t>(it - encounter_.order.begin())
          : 0;
}

auto GameState::ApplyCommand(const Command& command) -> std::optional<Event> {
  return std::visit(
      [this](const auto& cmd) -> std::optional<Event> {
        using T = std::decay_t<decltype(cmd)>;
        if constexpr (std::is_same_v<T, MoveToken>) {
          if (!map_.Contains(cmd.target) || map_.IsBlocked(cmd.target)) {
            return std::nullopt;
          }
          Token* token = FindToken(cmd.id);
          if (token == nullptr || token->cell == cmd.target) {
            return std::nullopt;
          }
          const Cell from = token->cell;
          token->cell = cmd.target;
          return Event{
              TokenMoved{.id = token->id, .from = from, .to = cmd.target}};
        }
        return std::nullopt;
      },
      command);
}

auto GameState::ApplyEvent(const Event& event) -> void {
  std::visit(
      [this](const auto& ev) -> void {
        using T = std::decay_t<decltype(ev)>;
        if constexpr (std::is_same_v<T, TokenMoved>) {
          if (Token* token = FindToken(ev.id)) {
            token->cell = ev.to;
          }
        }
      },
      event);
}

}  // namespace dnd
