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

// Авторитетное состояние мира. В локальной игре работает в том же процессе
// рядом с клиентом; по сети живёт только на хосте.
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
                              int max_hp = 10, std::uint32_t color = 0,
                              int ac = 10, int speed = 30,
                              std::string notes = {}, int attack_bonus = 0,
                              int save_bonus = 0, std::string damage = {})
      -> TokenId;

  // Применяет команду клиента; возвращает событие для рассылки, если команда
  // изменила мир.
  [[nodiscard]] auto ApplyCommand(const Command& command)
      -> std::optional<Event>;

  // Обновление вида на клиенте: двигать токены разрешено только событиями.
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
  [[nodiscard]] auto UpdateTokenSheet(TokenId id, int ac, int speed,
                                      std::string notes, int attack_bonus,
                                      int save_bonus, std::string damage,
                                      int temp_hp) -> std::optional<Token>;
  [[nodiscard]] auto UpdateTokenCombat(TokenId id,
                                       std::vector<std::string> conditions)
      -> std::optional<Token>;

  // Боевой порядок.
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
