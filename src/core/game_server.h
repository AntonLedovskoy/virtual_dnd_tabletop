#pragma once

#include <cstdint>
#include <random>
#include <string>
#include <vector>

#include "core/command.h"
#include "core/event.h"
#include "core/game_state.h"
#include "core/snapshot.h"
#include "core/token.h"
#include "core/visibility.h"

namespace dnd {

struct CommandOutcome {
  bool accepted = false;
  std::string reject_reason;
  std::vector<Event> events;
};

// Авторитетная сессия: владеет миром, проверяет команды с учётом владения
// токенами, бросает кубы и хранит срез видимости локального игрока.
// Сетевая игра запускает тот же класс на хосте.
class GameServer {
 public:
  // Обзор партии дают только токены персонажей (kPlayer), не NPC и не монстры.
  GameServer(GameSnapshot snapshot, int sight_radius,
             std::uint32_t rng_seed = std::random_device{}());

  // Заменяет мир (загрузка/сохранение) и пересчитывает видимость.
  auto Reset(GameSnapshot snapshot) -> void;

  // Применяет команду. `actor` — имя игрока из handshake, `can_control_all`
  // истинно для DM. Токены с непустым owner может менять только владелец;
  // DM может менять всё.
  [[nodiscard]] auto HandleCommand(const Command& command,
                                   const std::string& actor,
                                   bool can_control_all) -> CommandOutcome;

  [[nodiscard]] auto State() const -> const GameState& { return state_; }
  [[nodiscard]] auto Snapshot() const -> GameSnapshot {
    return state_.Snapshot();
  }
  // Вид игрока: только токены в пределах видимости партии.
  [[nodiscard]] auto ClientSnapshot() const -> GameSnapshot;
  // Вид DM: весь мир.
  [[nodiscard]] auto DmSnapshot() const -> GameSnapshot {
    return state_.Snapshot();
  }
  [[nodiscard]] auto FogState() const -> FogSnapshot { return fog_.Snapshot(); }
  // DM видит все клетки, поэтому туману нечего скрывать.
  [[nodiscard]] auto FullFogState() const -> FogSnapshot;

  // Можно ли отправлять это событие клиенту игрока.
  [[nodiscard]] auto EventVisibleToPlayers(const Event& event) const -> bool;

 private:
  [[nodiscard]] static auto CanControl(const Token& token,
                                       const std::string& actor,
                                       bool can_control_all) -> bool;

  [[nodiscard]] auto HandleMove(const MoveToken& command,
                                const std::string& actor, bool can_control_all)
      -> CommandOutcome;
  [[nodiscard]] static auto HandleSay(const Say& command,
                                      const std::string& actor)
      -> CommandOutcome;
  [[nodiscard]] auto HandleRoll(const RollDice& command,
                                const std::string& actor) -> CommandOutcome;
  [[nodiscard]] auto HandleRollExpression(const RollExpression& command,
                                          const std::string& actor)
      -> CommandOutcome;
  [[nodiscard]] auto HandleAddToken(const AddToken& command,
                                    bool can_control_all) -> CommandOutcome;
  [[nodiscard]] auto HandleRemoveToken(const RemoveToken& command,
                                       bool can_control_all) -> CommandOutcome;
  [[nodiscard]] auto HandleSetHp(const SetTokenHp& command,
                                 const std::string& actor, bool can_control_all)
      -> CommandOutcome;
  [[nodiscard]] auto HandleDamage(const DamageToken& command,
                                  const std::string& actor,
                                  bool can_control_all) -> CommandOutcome;
  [[nodiscard]] auto HandleHeal(const HealToken& command,
                                const std::string& actor, bool can_control_all)
      -> CommandOutcome;
  [[nodiscard]] auto HandleRest(const RestParty& command, bool can_control_all)
      -> CommandOutcome;
  [[nodiscard]] auto HandleAoeDamage(const AoeDamage& command,
                                     bool can_control_all) -> CommandOutcome;
  [[nodiscard]] auto HandleDeathSave(const DeathSave& command,
                                     const std::string& actor,
                                     bool can_control_all) -> CommandOutcome;
  [[nodiscard]] auto HandleSetInitiative(const SetInitiative& command,
                                         const std::string& actor,
                                         bool can_control_all)
      -> CommandOutcome;
  [[nodiscard]] auto HandleClaimToken(const ClaimToken& command,
                                      const std::string& actor)
      -> CommandOutcome;
  [[nodiscard]] auto HandleReleaseToken(const ReleaseToken& command,
                                        const std::string& actor)
      -> CommandOutcome;
  [[nodiscard]] auto HandleSetTokenOwner(const SetTokenOwner& command,
                                         bool can_control_all)
      -> CommandOutcome;
  [[nodiscard]] auto HandleRenameToken(const RenameToken& command,
                                       const std::string& actor,
                                       bool can_control_all) -> CommandOutcome;
  [[nodiscard]] auto HandleSetTokenMaxHp(const SetTokenMaxHp& command,
                                         const std::string& actor,
                                         bool can_control_all)
      -> CommandOutcome;
  [[nodiscard]] auto HandleSetTokenColor(const SetTokenColor& command,
                                         const std::string& actor,
                                         bool can_control_all)
      -> CommandOutcome;
  [[nodiscard]] auto HandleSetTokenPortrait(const SetTokenPortrait& command,
                                            const std::string& actor,
                                            bool can_control_all)
      -> CommandOutcome;
  [[nodiscard]] auto HandleUpdateTokenSheet(const UpdateTokenSheet& command,
                                            const std::string& actor,
                                            bool can_control_all)
      -> CommandOutcome;
  [[nodiscard]] auto HandleUpdateTokenCombat(const UpdateTokenCombat& command,
                                             const std::string& actor,
                                             bool can_control_all)
      -> CommandOutcome;
  [[nodiscard]] auto HandleStartEncounter(bool can_control_all)
      -> CommandOutcome;
  [[nodiscard]] auto HandleEndEncounter(bool can_control_all) -> CommandOutcome;
  [[nodiscard]] auto HandleSetAoe(const SetAoe& command, bool can_control_all)
      -> CommandOutcome;
  // Метка «сюда»: любой участник, клетка проверяется по карте.
  [[nodiscard]] auto HandlePing(const PingAt& command, const std::string& actor)
      -> CommandOutcome;
  // Подложку карты меняет только DM: это общий вид стола.
  [[nodiscard]] auto HandleSetMapBackground(const SetMapBackground& command,
                                            bool can_control_all)
      -> CommandOutcome;
  [[nodiscard]] auto HandleNextTurn(bool can_control_all) -> CommandOutcome;

  [[nodiscard]] auto FindToken(TokenId id) const -> const Token*;
  auto RecomputeFog() -> void;

  GameState state_;
  FogOfWar fog_;
  int sight_radius_;
  std::mt19937 rng_;
};

}  // namespace dnd
