#pragma once

#include <optional>
#include <string>
#include <vector>

#include "core/command.h"
#include "core/encounter.h"
#include "core/event.h"
#include "core/grid_map.h"
#include "core/snapshot.h"
#include "core/token.h"

namespace dnd {

// Authoritative world state. Local play runs it in-process next to the client;
// over the network it will live on the host only.
class GameState {
 public:
  explicit GameState(GridMap map);
  explicit GameState(GameSnapshot snapshot);

  [[nodiscard]] auto Map() const -> const GridMap& { return map_; }
  [[nodiscard]] auto Tokens() const -> const std::vector<Token>& {
    return tokens_;
  }
  [[nodiscard]] auto Snapshot() const -> GameSnapshot;
  [[nodiscard]] auto TokenCopy(TokenId id) const -> std::optional<Token>;

  [[nodiscard]] auto AddToken(std::string name, Cell cell,
                              std::string owner = {},
                              TokenKind kind = TokenKind::kPlayer,
                              int max_hp = 10, std::uint32_t color = 0)
      -> TokenId;

  // Applies a client command; returns the event to broadcast when the command
  // changed the world.
  [[nodiscard]] auto ApplyCommand(const Command& command)
      -> std::optional<Event>;

  // Client-side view update: only events are allowed to move tokens.
  auto ApplyEvent(const Event& event) -> void;

  auto RemoveToken(TokenId id) -> bool;
  [[nodiscard]] auto SetTokenHp(TokenId id, int hp) -> std::optional<Token>;
  [[nodiscard]] auto SetInitiative(TokenId id, int value)
      -> std::optional<Token>;
  [[nodiscard]] auto SetTokenOwner(TokenId id, std::string owner)
      -> std::optional<Token>;
  [[nodiscard]] auto RenameToken(TokenId id, std::string name)
      -> std::optional<Token>;
  [[nodiscard]] auto SetTokenMaxHp(TokenId id, int max_hp)
      -> std::optional<Token>;
  [[nodiscard]] auto SetTokenColor(TokenId id, std::uint32_t color)
      -> std::optional<Token>;

  // Combat order.
  [[nodiscard]] auto EncounterState() const -> const Encounter& {
    return encounter_;
  }
  [[nodiscard]] auto ActiveTokenId() const -> TokenId;
  [[nodiscard]] auto StartEncounter() -> bool;
  auto EndEncounter() -> void;
  [[nodiscard]] auto NextTurn() -> bool;

 private:
  auto FindToken(TokenId id) -> Token*;
  [[nodiscard]] auto FindToken(TokenId id) const -> const Token*;
  auto RebuildOrder(TokenId keep) -> void;

  GridMap map_;
  std::vector<Token> tokens_;
  TokenId next_token_id_ = 1;
  Encounter encounter_;
};

}  // namespace dnd
