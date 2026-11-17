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

// Authoritative session: owns the world, validates commands against token
// ownership, rolls dice and keeps the visibility slice of the local player.
// Network play runs the same class on the host.
class GameServer {
 public:
  // All player-kind tokens provide party vision; NPCs and monsters never do.
  GameServer(GameSnapshot snapshot, int sight_radius,
             std::uint32_t rng_seed = std::random_device{}());

  // Replaces the world (load/save) and recomputes visibility.
  auto Reset(GameSnapshot snapshot) -> void;

  // Applies a command. `actor` is the player name from the handshake and
  // `can_control_all` is true for the DM. Tokens with a non-empty owner can
  // only be changed by that owner; the DM can change everything.
  [[nodiscard]] auto HandleCommand(const Command& command,
                                   const std::string& actor,
                                   bool can_control_all) -> CommandOutcome;

  [[nodiscard]] auto State() const -> const GameState& { return state_; }
  [[nodiscard]] auto Snapshot() const -> GameSnapshot {
    return state_.Snapshot();
  }
  // Player view: only tokens inside party line of sight.
  [[nodiscard]] auto ClientSnapshot() const -> GameSnapshot;
  // DM view: the whole world.
  [[nodiscard]] auto DmSnapshot() const -> GameSnapshot {
    return state_.Snapshot();
  }
  [[nodiscard]] auto FogState() const -> FogSnapshot { return fog_.Snapshot(); }
  // DM sees every cell, so the fog overlay has nothing to hide.
  [[nodiscard]] auto FullFogState() const -> FogSnapshot;

  // Whether a player's client is allowed to receive this event.
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
  [[nodiscard]] auto HandleAddToken(const AddToken& command,
                                    bool can_control_all) -> CommandOutcome;
  [[nodiscard]] auto HandleRemoveToken(const RemoveToken& command,
                                       bool can_control_all) -> CommandOutcome;
  [[nodiscard]] auto HandleSetHp(const SetTokenHp& command,
                                 const std::string& actor, bool can_control_all)
      -> CommandOutcome;
  [[nodiscard]] auto HandleSetInitiative(const SetInitiative& command,
                                         const std::string& actor,
                                         bool can_control_all)
      -> CommandOutcome;
  [[nodiscard]] auto HandleClaimToken(const ClaimToken& command,
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
  [[nodiscard]] auto HandleStartEncounter(bool can_control_all)
      -> CommandOutcome;
  [[nodiscard]] auto HandleEndEncounter(bool can_control_all) -> CommandOutcome;
  [[nodiscard]] auto HandleNextTurn(bool can_control_all) -> CommandOutcome;

  [[nodiscard]] auto FindToken(TokenId id) const -> const Token*;
  auto RecomputeFog() -> void;

  GameState state_;
  FogOfWar fog_;
  int sight_radius_;
  std::mt19937 rng_;
};

}  // namespace dnd
