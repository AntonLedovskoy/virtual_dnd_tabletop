#include <algorithm>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "core/aoe.h"
#include "core/bestiary.h"
#include "core/campaign.h"
#include "core/campaign_io.h"
#include "core/client_view.h"
#include "core/conditions.h"
#include "core/dice.h"
#include "core/game_server.h"
#include "core/game_state.h"
#include "core/grid_map.h"
#include "core/local_transport.h"
#include "core/password.h"
#include "core/scene_io.h"
#include "core/serialization.h"
#include "core/session_log.h"
#include "core/sha256.h"
#include "core/utf8.h"
#include "core/visibility.h"

namespace {

auto g_failures = 0;

auto Expect(bool condition, const char* name) -> void {
  if (!condition) {
    std::fprintf(stderr, "FAILED: %s\n", name);
    ++g_failures;
  }
}

auto CellAt(int x, int y) -> dnd::Cell { return dnd::Cell{.x = x, .y = y}; }

auto HasCell(const std::vector<dnd::Cell>& cells, dnd::Cell cell) -> bool {
  return std::ranges::find(cells, cell) != cells.end();
}

// Токен для проверок: отсутствие токена — уже ошибка теста, поэтому вместо
// разыменования optional возвращается пустой токен.
auto TokenOf(const dnd::GameState& state, dnd::TokenId id) -> dnd::Token {
  const std::optional<dnd::Token> token = state.TokenCopy(id);
  Expect(token.has_value(), "the token exists");
  return token.value_or(dnd::Token{});
}

auto FogVisibleAt(const dnd::FogSnapshot& fog, int x, int y) -> bool {
  return fog.visible[static_cast<std::size_t>(y) *
                         static_cast<std::size_t>(fog.width) +
                     static_cast<std::size_t>(x)] != 0;
}

auto TestGridMap() -> void {
  dnd::GridMap map(4, 3);
  Expect(map.Contains(CellAt(0, 0)), "map contains origin");
  Expect(map.Contains(CellAt(3, 2)), "map contains last cell");
  Expect(!map.Contains(CellAt(4, 0)), "map rejects x overflow");
  Expect(!map.Contains(CellAt(-1, 0)), "map rejects negative x");
  Expect(!map.IsBlocked(CellAt(1, 1)), "cell starts free");
  map.SetBlocked(CellAt(1, 1), true);
  Expect(map.IsBlocked(CellAt(1, 1)), "cell becomes blocked");
  Expect(!map.IsBlocked(CellAt(9, 9)), "outside cell is not blocked");

  dnd::GridMap reachable_map(5, 5);
  reachable_map.SetBlocked(CellAt(2, 2), true);
  const auto reachable = dnd::ReachableCells(reachable_map, CellAt(0, 0), 2);
  Expect(HasCell(reachable, CellAt(0, 0)), "movement range includes origin");
  Expect(HasCell(reachable, CellAt(2, 0)),
         "movement range includes open cells");
  Expect(!HasCell(reachable, CellAt(2, 2)), "movement range excludes walls");
  Expect(!HasCell(reachable, CellAt(3, 3)), "movement range respects radius");

  dnd::GridMap corner_map(3, 3);
  corner_map.SetBlocked(CellAt(1, 0), true);
  corner_map.SetBlocked(CellAt(0, 1), true);
  const auto corner_range = dnd::ReachableCells(corner_map, CellAt(0, 0), 2);
  Expect(!HasCell(corner_range, CellAt(1, 1)),
         "movement range does not cut through a sealed corner");

  Expect(dnd::PathCostCells(reachable_map, CellAt(0, 0), CellAt(2, 0)) == 2,
         "path cost counts straight steps");
  Expect(dnd::PathCostCells(reachable_map, CellAt(0, 0), CellAt(0, 0)) == 0,
         "path cost to the same cell is zero");
  Expect(!dnd::PathCostCells(reachable_map, CellAt(0, 0), CellAt(2, 2)),
         "path cost skips walls");
  Expect(!dnd::PathCostCells(corner_map, CellAt(0, 0), CellAt(1, 1)),
         "path cost does not cut through a sealed corner");
}

auto TestMoveRules() -> void {
  dnd::GridMap map(5, 5);
  map.SetBlocked(CellAt(2, 2), true);
  dnd::GameState state(map);
  const dnd::TokenId id = state.AddToken("hero", CellAt(0, 0));

  Expect(state.Tokens().front().cell == CellAt(0, 0), "token starts placed");
  Expect(!state.ApplyCommand(dnd::MoveToken{.id = id, .target = CellAt(2, 2)})
              .has_value(),
         "move into wall is rejected");
  Expect(!state.ApplyCommand(dnd::MoveToken{.id = id, .target = CellAt(7, 7)})
              .has_value(),
         "move outside map is rejected");
  Expect(!state.ApplyCommand(dnd::MoveToken{.id = 999, .target = CellAt(1, 1)})
              .has_value(),
         "move of unknown token is rejected");

  const auto moved =
      state.ApplyCommand(dnd::MoveToken{.id = id, .target = CellAt(1, 1)});
  Expect(moved.has_value(), "valid move produces an event");
  if (moved.has_value()) {
    state.ApplyEvent(*moved);
  }
  Expect(state.Tokens().front().cell == CellAt(1, 1), "event moves the token");
}

auto TestFogLineOfSight() -> void {
  dnd::GridMap map(5, 1);
  map.SetBlocked(CellAt(3, 0), true);

  dnd::FogOfWar fog(5, 1);
  fog.Recompute(map, {CellAt(0, 0)}, 4);

  Expect(fog.IsVisible(CellAt(0, 0)), "observer cell is visible");
  Expect(fog.IsVisible(CellAt(2, 0)), "open cell before wall is visible");
  Expect(fog.IsVisible(CellAt(3, 0)), "wall tile itself is visible");
  Expect(!fog.IsVisible(CellAt(4, 0)), "cell behind wall is hidden");
  Expect(fog.IsExplored(CellAt(2, 0)), "visible cell becomes explored");
  Expect(!fog.IsExplored(CellAt(4, 0)), "hidden cell is not explored");
}

auto TestFogDiagonalSeal() -> void {
  dnd::GridMap sealed(4, 4);
  sealed.SetBlocked(CellAt(1, 0), true);
  sealed.SetBlocked(CellAt(0, 1), true);
  dnd::FogOfWar fog(4, 4);
  fog.Recompute(sealed, {CellAt(0, 0)}, 4);
  Expect(fog.IsVisible(CellAt(0, 0)), "observer cell is visible");
  Expect(!fog.IsVisible(CellAt(1, 1)),
         "walls touching by a corner seal diagonal sight");
  Expect(!fog.IsVisible(CellAt(2, 2)),
         "nothing behind a sealed diagonal corner is visible");

  dnd::GridMap open(4, 4);
  open.SetBlocked(CellAt(1, 0), true);
  dnd::FogOfWar open_fog(4, 4);
  open_fog.Recompute(open, {CellAt(0, 0)}, 4);
  Expect(open_fog.IsVisible(CellAt(1, 1)),
         "a single corner wall still lets sight around it");
}

auto TestFogExploredMemory() -> void {
  const dnd::GridMap map(6, 1);
  dnd::FogOfWar fog(6, 1);

  fog.Recompute(map, {CellAt(0, 0)}, 2);
  Expect(fog.IsVisible(CellAt(1, 0)), "cell visible from first position");

  fog.Recompute(map, {CellAt(5, 0)}, 1);
  Expect(!fog.IsVisible(CellAt(1, 0)), "old cell is no longer visible");
  Expect(fog.IsExplored(CellAt(1, 0)), "old cell stays explored");
  Expect(fog.IsVisible(CellAt(5, 0)), "new observer cell is visible");

  fog.Recompute(map, {CellAt(99, 99)}, 5);
  Expect(!fog.IsVisible(CellAt(5, 0)), "out-of-map observers reveal nothing");
}

auto TestServerVisibility() -> void {
  dnd::GridMap map(10, 1);
  map.SetBlocked(CellAt(5, 0), true);
  const dnd::GameSnapshot snapshot{
      .map = map,
      .tokens = {dnd::Token{.id = 1,
                            .name = "hero",
                            .owner = "alice",
                            .kind = dnd::TokenKind::kPlayer,
                            .cell = CellAt(0, 0)},
                 dnd::Token{.id = 2,
                            .name = "goblin",
                            .kind = dnd::TokenKind::kMonster,
                            .cell = CellAt(8, 0)}},
      .next_token_id = 3,
  };

  dnd::GameServer server(snapshot, 10);

  const dnd::GameSnapshot client = server.ClientSnapshot();
  Expect(client.tokens.size() == 1, "hidden token is not in client snapshot");
  Expect(client.tokens.front().id == 1, "visible token is in client snapshot");
  Expect(server.DmSnapshot().tokens.size() == 2, "DM snapshot sees everything");

  dnd::ClientView view;
  view.Reset(server.ClientSnapshot(), server.FogState());
  Expect(view.Fog().IsVisible(CellAt(0, 0)), "view receives server fog");
  Expect(!view.Fog().IsVisible(CellAt(8, 0)), "goblin cell is hidden");

  const dnd::CommandOutcome hidden = server.HandleCommand(
      dnd::MoveToken{.id = 2, .target = CellAt(9, 0)}, "alice", false);
  Expect(hidden.accepted, "free token move is accepted");
  bool hidden_move_visible = false;
  for (const dnd::Event& event : hidden.events) {
    if (std::holds_alternative<dnd::TokenMoved>(event)) {
      hidden_move_visible = server.EventVisibleToPlayers(event);
    }
    view.Apply(event);
  }
  Expect(!hidden_move_visible, "hidden move is not visible to players");
  Expect(view.Tokens().size() == 1, "client view still hides the goblin");

  const dnd::CommandOutcome own = server.HandleCommand(
      dnd::MoveToken{.id = 1, .target = CellAt(1, 0)}, "alice", false);
  Expect(own.accepted, "visible move is accepted");
  bool own_move_visible = false;
  for (const dnd::Event& event : own.events) {
    if (std::holds_alternative<dnd::TokenMoved>(event)) {
      own_move_visible = server.EventVisibleToPlayers(event);
    }
    view.Apply(event);
  }
  Expect(own_move_visible, "visible move is visible to players");
  Expect(view.Tokens().front().cell == CellAt(1, 0), "client view moves token");

  // Токен, скрытый при входе клиента, должен появиться, когда он попадёт
  // в поле зрения партии.
  const dnd::CommandOutcome reveal = server.HandleCommand(
      dnd::MoveToken{.id = 2, .target = CellAt(4, 0)}, "alice", false);
  Expect(reveal.accepted, "goblin move into sight is accepted");
  bool view_refreshed = false;
  for (const dnd::Event& event : reveal.events) {
    if (std::holds_alternative<dnd::ViewUpdated>(event)) {
      view_refreshed = true;
    }
    view.Apply(event);
  }
  Expect(view_refreshed, "moves carry a full view refresh");
  Expect(view.Tokens().size() == 2,
         "revealed token appears in the client view");
}

auto TestOwnership() -> void {
  const dnd::GridMap map(20, 1);
  const dnd::GameSnapshot snapshot{
      .map = map,
      .tokens =
          {
              dnd::Token{.id = 1,
                         .name = "hero",
                         .owner = "alice",
                         .kind = dnd::TokenKind::kPlayer,
                         .cell = CellAt(0, 0)},
              dnd::Token{.id = 2,
                         .name = "goblin",
                         .owner = "dm",
                         .kind = dnd::TokenKind::kMonster,
                         .cell = CellAt(15, 0)},
              dnd::Token{.id = 3,
                         .name = "ally",
                         .kind = dnd::TokenKind::kPlayer,
                         .cell = CellAt(4, 0)},
          },
      .next_token_id = 4,
  };
  dnd::GameServer server(snapshot, 3);

  Expect(server.ClientSnapshot().tokens.size() == 2,
         "far goblin is hidden from players");
  Expect(server.DmSnapshot().tokens.size() == 3, "DM sees the goblin");

  const dnd::CommandOutcome owner_move = server.HandleCommand(
      dnd::MoveToken{.id = 1, .target = CellAt(1, 0)}, "alice", false);
  Expect(owner_move.accepted, "owner may move their token");

  const dnd::CommandOutcome foreign_move = server.HandleCommand(
      dnd::MoveToken{.id = 2, .target = CellAt(16, 0)}, "alice", false);
  Expect(!foreign_move.accepted, "player cannot move a DM token");
  Expect(!foreign_move.reject_reason.empty(), "rejection carries a reason");

  const dnd::CommandOutcome dm_move = server.HandleCommand(
      dnd::MoveToken{.id = 2, .target = CellAt(16, 0)}, "", true);
  Expect(dm_move.accepted, "DM may move any token");

  const dnd::CommandOutcome locked_move = server.HandleCommand(
      dnd::MoveToken{.id = 3, .target = CellAt(5, 0)}, "bob", false);
  Expect(!locked_move.accepted, "unclaimed characters cannot be moved");

  Expect(server.HandleCommand(dnd::ClaimToken{.id = 3}, "bob", false).accepted,
         "a free character can be claimed before moving");
  const dnd::CommandOutcome free_move = server.HandleCommand(
      dnd::MoveToken{.id = 3, .target = CellAt(5, 0)}, "bob", false);
  Expect(free_move.accepted, "the claimed character can be moved by its owner");

  const dnd::CommandOutcome other_move = server.HandleCommand(
      dnd::MoveToken{.id = 1, .target = CellAt(2, 0)}, "bob", false);
  Expect(!other_move.accepted, "players cannot move somebody else's token");
}

auto TestClaimAndTokenAdmin() -> void {
  const dnd::GridMap map(12, 12);
  const dnd::GameSnapshot snapshot{
      .map = map,
      .tokens =
          {
              dnd::Token{.id = 1,
                         .name = "hero",
                         .kind = dnd::TokenKind::kPlayer,
                         .cell = CellAt(1, 1)},
              dnd::Token{.id = 2,
                         .name = "goblin",
                         .owner = "dm",
                         .kind = dnd::TokenKind::kMonster,
                         .cell = CellAt(2, 2)},
              dnd::Token{.id = 3,
                         .name = "second",
                         .kind = dnd::TokenKind::kPlayer,
                         .cell = CellAt(3, 3)},
          },
      .next_token_id = 4,
  };
  dnd::GameServer server(snapshot, 8, 5);

  const dnd::CommandOutcome monster_claim =
      server.HandleCommand(dnd::ClaimToken{.id = 2}, "alice", false);
  Expect(!monster_claim.accepted, "monsters cannot be claimed");

  const dnd::CommandOutcome locked_move = server.HandleCommand(
      dnd::MoveToken{.id = 1, .target = CellAt(2, 1)}, "alice", false);
  Expect(!locked_move.accepted, "unclaimed character is locked");
  Expect(locked_move.reject_reason.find("заберите") != std::string::npos,
         "locked character rejection explains the claim");

  const dnd::CommandOutcome claim =
      server.HandleCommand(dnd::ClaimToken{.id = 1}, "alice", false);
  Expect(claim.accepted, "a player claims a free character");
  Expect(!claim.events.empty() &&
             std::holds_alternative<dnd::TokenUpdated>(claim.events.front()),
         "claim broadcasts a token update");
  Expect(TokenOf(server.State(), 1).owner == "alice",
         "claim stores the player name");
  Expect(
      !server.HandleCommand(dnd::ClaimToken{.id = 3}, "alice", false).accepted,
      "a player cannot claim a second character");

  Expect(!server
              .HandleCommand(dnd::MoveToken{.id = 1, .target = CellAt(2, 1)},
                             "bob", false)
              .accepted,
         "another player cannot move the claimed token");
  Expect(server
             .HandleCommand(dnd::MoveToken{.id = 1, .target = CellAt(2, 1)},
                            "alice", false)
             .accepted,
         "the owner can move the claimed token");
  Expect(!server.HandleCommand(dnd::ClaimToken{.id = 1}, "bob", false).accepted,
         "a claimed token cannot be stolen");

  const dnd::CommandOutcome assign = server.HandleCommand(
      dnd::SetTokenOwner{.id = 1, .owner = "bob"}, "", true);
  Expect(assign.accepted, "the DM can reassign a character");
  Expect(
      !server.HandleCommand(dnd::ClaimToken{.id = 1}, "alice", false).accepted,
      "the previous owner loses the token after reassignment");
  Expect(server.HandleCommand(dnd::ClaimToken{.id = 1}, "bob", false).accepted,
         "the new owner can claim the reassigned token");

  Expect(!server
              .HandleCommand(dnd::SetTokenOwner{.id = 1, .owner = "alice"},
                             "alice", false)
              .accepted,
         "players cannot assign owners");
  Expect(
      server.HandleCommand(dnd::SetTokenOwner{.id = 1, .owner = ""}, "", true)
          .accepted,
      "the DM can release a character");
  Expect(
      server.HandleCommand(dnd::ClaimToken{.id = 1}, "alice", false).accepted,
      "a released character can be claimed again");

  const dnd::CommandOutcome rename = server.HandleCommand(
      dnd::RenameToken{.id = 1, .name = "Алиса"}, "alice", false);
  Expect(rename.accepted, "the owner can rename the token");
  Expect(TokenOf(server.State(), 1).name == "Алиса",
         "the renamed value is stored");
  Expect(!server
              .HandleCommand(dnd::RenameToken{.id = 1, .name = "Боб"}, "bob",
                             false)
              .accepted,
         "other players cannot rename the token");
  Expect(
      !server
           .HandleCommand(dnd::RenameToken{.id = 1, .name = ""}, "alice", false)
           .accepted,
      "empty names are rejected");

  Expect(
      server.HandleCommand(dnd::SetTokenHp{.id = 1, .hp = 10}, "alice", false)
          .accepted,
      "the owner can set hp");
  Expect(server
             .HandleCommand(dnd::SetTokenMaxHp{.id = 1, .max_hp = 5}, "alice",
                            false)
             .accepted,
         "the owner can set max hp");
  Expect(TokenOf(server.State(), 1).hp == 5,
         "hp is clamped to the new maximum");
  Expect(!server
              .HandleCommand(dnd::SetTokenMaxHp{.id = 1, .max_hp = 0}, "alice",
                             false)
              .accepted,
         "a zero maximum is rejected");

  const dnd::CommandOutcome color = server.HandleCommand(
      dnd::SetTokenColor{.id = 1, .color = 0xC85046U}, "alice", false);
  Expect(color.accepted, "the owner can set the colour");
  Expect(TokenOf(server.State(), 1).color == 0xC85046U, "the colour is stored");

  const dnd::CommandOutcome sheet =
      server.HandleCommand(dnd::UpdateTokenSheet{.id = 1,
                                                 .ac = 16,
                                                 .speed = 9,
                                                 .notes = "Атака: +5, 1d8+3",
                                                 .attack_bonus = 5,
                                                 .save_bonus = 2,
                                                 .damage = "1d8+3",
                                                 .temp_hp = 4},
                           "alice", false);
  Expect(sheet.accepted, "the owner can edit the character sheet");
  Expect(TokenOf(server.State(), 1).ac == 16, "КД сохраняется");
  Expect(TokenOf(server.State(), 1).speed == 9, "speed is stored");
  Expect(TokenOf(server.State(), 1).notes == "Атака: +5, 1d8+3",
         "notes are stored");
  Expect(TokenOf(server.State(), 1).attack_bonus == 5,
         "attack bonus is stored");
  Expect(TokenOf(server.State(), 1).save_bonus == 2, "save bonus is stored");
  Expect(TokenOf(server.State(), 1).damage == "1d8+3", "damage is stored");
  Expect(TokenOf(server.State(), 1).temp_hp == 4, "temporary HP are stored");
  Expect(!server
              .HandleCommand(
                  dnd::UpdateTokenSheet{
                      .id = 1, .ac = 12, .speed = 9, .notes = "чужое"},
                  "bob", false)
              .accepted,
         "other players cannot edit the sheet");
  Expect(
      !server
           .HandleCommand(
               dnd::UpdateTokenSheet{.id = 1, .ac = 0, .speed = 9, .notes = ""},
               "alice", false)
           .accepted,
      "некорректный КД отвергается");
  Expect(!server
              .HandleCommand(
                  dnd::UpdateTokenSheet{
                      .id = 1, .ac = 12, .speed = 9, .temp_hp = 1001},
                  "alice", false)
              .accepted,
         "too much temporary HP is rejected");

  const dnd::CommandOutcome conditions = server.HandleCommand(
      dnd::UpdateTokenCombat{.id = 1,
                             .conditions = {dnd::Condition{"сбит с ног"},
                                            dnd::Condition{"отравлен"}}},
      "alice", false);
  Expect(conditions.accepted, "the owner can update conditions");
  Expect(TokenOf(server.State(), 1).conditions.size() == 2,
         "conditions are stored");
  Expect(!server
              .HandleCommand(
                  dnd::UpdateTokenCombat{
                      .id = 1, .conditions = {dnd::Condition{"чужое"}}},
                  "bob", false)
              .accepted,
         "other players cannot update conditions");
  Expect(!server
              .HandleCommand(
                  dnd::UpdateTokenCombat{.id = 1,
                                         .conditions = {dnd::Condition{""}}},
                  "alice", false)
              .accepted,
         "empty condition is rejected");

  Expect(
      !server.HandleCommand(dnd::ReleaseToken{.id = 1}, "bob", false).accepted,
      "another player cannot release the character");
  Expect(
      server.HandleCommand(dnd::ReleaseToken{.id = 1}, "alice", false).accepted,
      "the owner can release the character");
  Expect(TokenOf(server.State(), 1).owner.empty(),
         "released character becomes free");
  Expect(
      server.HandleCommand(dnd::ClaimToken{.id = 3}, "alice", false).accepted,
      "the player can claim another character after releasing");
}

auto TestDamageHealRest() -> void {
  const dnd::GridMap map(8, 8);
  const dnd::GameSnapshot snapshot{
      .map = map,
      .tokens =
          {
              dnd::Token{.id = 1,
                         .name = "hero",
                         .owner = "alice",
                         .hp = 20,
                         .max_hp = 20,
                         .temp_hp = 5,
                         .kind = dnd::TokenKind::kPlayer,
                         .cell = CellAt(1, 1)},
              dnd::Token{.id = 2,
                         .name = "mage",
                         .owner = "bob",
                         .hp = 0,
                         .max_hp = 10,
                         .conditions = {dnd::Condition{"без сознания"},
                                        dnd::Condition{"отравлен"}},
                         .kind = dnd::TokenKind::kPlayer,
                         .cell = CellAt(2, 2)},
          },
      .next_token_id = 3,
  };
  dnd::GameServer server(snapshot, 6, 7);

  dnd::CommandOutcome hit = server.HandleCommand(
      dnd::DamageToken{.id = 1, .amount = 7}, "alice", false);
  Expect(hit.accepted, "the owner can damage their character");
  Expect(TokenOf(server.State(), 1).temp_hp == 0,
         "temporary hp absorbs damage first");
  Expect(TokenOf(server.State(), 1).hp == 18,
         "only the rest of the damage reaches hp");

  hit = server.HandleCommand(dnd::DamageToken{.id = 1, .amount = 30}, "alice",
                             false);
  Expect(hit.accepted, "damage beyond hp is accepted");
  const dnd::Token down = TokenOf(server.State(), 1);
  Expect(down.hp == 0, "hp stops at zero");
  Expect(std::ranges::any_of(down.conditions,
                             [](const dnd::Condition& condition) -> bool {
                               return condition.name == dnd::kUnconscious;
                             }),
         "zero hp adds the unconscious condition");

  const dnd::CommandOutcome heal = server.HandleCommand(
      dnd::HealToken{.id = 1, .amount = 50}, "alice", false);
  Expect(heal.accepted, "the owner can heal their character");
  const dnd::Token healed = TokenOf(server.State(), 1);
  Expect(healed.hp == 20, "healing stops at max hp");
  Expect(!std::ranges::any_of(healed.conditions,
                              [](const dnd::Condition& condition) -> bool {
                                return condition.name == dnd::kUnconscious;
                              }),
         "healing removes the unconscious condition");

  Expect(
      !server.HandleCommand(dnd::RestParty{.long_rest = true}, "alice", false)
           .accepted,
      "only the DM declares a rest");
  Expect(server.HandleCommand(dnd::RestParty{}, "", true).accepted,
         "the DM declares a short rest");
  const dnd::Token rested = TokenOf(server.State(), 2);
  Expect(rested.hp == 5, "short rest restores half of max hp");
  Expect(rested.conditions.size() == 1 &&
             rested.conditions.front().name == "отравлен",
         "short rest wakes the character but keeps other conditions");

  Expect(server.HandleCommand(dnd::RestParty{.long_rest = true}, "", true)
             .accepted,
         "the DM declares a long rest");
  const dnd::Token restored = TokenOf(server.State(), 2);
  Expect(restored.hp == restored.max_hp && restored.temp_hp == 0 &&
             restored.conditions.empty(),
         "long rest restores hp and clears conditions");
}

auto TestConcentration() -> void {
  const dnd::GridMap map(8, 8);
  const dnd::GameSnapshot snapshot{
      .map = map,
      .tokens =
          {
              dnd::Token{.id = 1,
                         .name = "calm",
                         .owner = "alice",
                         .save_bonus = 100,
                         .conditions = {dnd::Condition{"концентрация"}},
                         .kind = dnd::TokenKind::kPlayer,
                         .cell = CellAt(1, 1)},
              dnd::Token{.id = 2,
                         .name = "shaken",
                         .owner = "bob",
                         .save_bonus = -100,
                         .conditions = {dnd::Condition{"концентрация"}},
                         .kind = dnd::TokenKind::kPlayer,
                         .cell = CellAt(2, 2)},
          },
      .next_token_id = 3,
  };
  dnd::GameServer server(snapshot, 6, 9);

  Expect(
      server
          .HandleCommand(dnd::DamageToken{.id = 1, .amount = 7}, "alice", false)
          .accepted,
      "damage with concentration is accepted");
  Expect(TokenOf(server.State(), 1).conditions.size() == 1,
         "a successful save keeps concentration");

  Expect(
      server.HandleCommand(dnd::DamageToken{.id = 2, .amount = 7}, "bob", false)
          .accepted,
      "damage with concentration is accepted");
  Expect(TokenOf(server.State(), 2).conditions.empty(),
         "a failed concentration save removes the condition");
}

auto TestAoeDamage() -> void {
  const dnd::GridMap map(8, 8);
  const dnd::GameSnapshot snapshot{
      .map = map,
      .tokens =
          {
              dnd::Token{.id = 1,
                         .name = "brute",
                         .owner = "dm",
                         .hp = 50,
                         .max_hp = 50,
                         .save_bonus = -100,
                         .kind = dnd::TokenKind::kMonster,
                         .cell = CellAt(1, 1)},
              dnd::Token{.id = 2,
                         .name = "rogue",
                         .owner = "dm",
                         .hp = 50,
                         .max_hp = 50,
                         .save_bonus = 100,
                         .kind = dnd::TokenKind::kMonster,
                         .cell = CellAt(2, 2)},
              dnd::Token{.id = 3,
                         .name = "far",
                         .owner = "dm",
                         .hp = 50,
                         .max_hp = 50,
                         .kind = dnd::TokenKind::kMonster,
                         .cell = CellAt(6, 6)},
          },
      .next_token_id = 4,
  };
  dnd::GameServer server(snapshot, 6, 5);
  Expect(server
             .HandleCommand(dnd::SetAoe{.aoe =
                                            dnd::AoeTemplate{
                                                .shape = dnd::AoeShape::kCircle,
                                                .origin = CellAt(1, 1),
                                                .radius = 2,
                                            }},
                            "", true)
             .accepted,
         "the DM places an area template");
  Expect(server
             .HandleCommand(dnd::AoeDamage{.expression = "2d2", .save_dc = 15},
                            "", true)
             .accepted,
         "the DM damages tokens inside the template");
  const dnd::Token brute = TokenOf(server.State(), 1);
  const dnd::Token rogue = TokenOf(server.State(), 2);
  Expect(brute.hp < rogue.hp, "a failed save takes full damage");
  Expect(rogue.hp < 50, "a successful save still takes half damage");
  Expect(TokenOf(server.State(), 3).hp == 50,
         "tokens outside the template are untouched");
}

auto TestDeathSaves() -> void {
  const dnd::GridMap map(8, 8);
  const dnd::GameSnapshot snapshot{
      .map = map,
      .tokens =
          {
              dnd::Token{.id = 1,
                         .name = "doomed",
                         .owner = "alice",
                         .hp = 0,
                         .max_hp = 10,
                         .conditions = {dnd::Condition{"без сознания"}},
                         .kind = dnd::TokenKind::kPlayer,
                         .cell = CellAt(1, 1)},
              dnd::Token{.id = 2,
                         .name = "stable",
                         .owner = "bob",
                         .hp = 0,
                         .max_hp = 10,
                         .conditions = {dnd::Condition{"без сознания"}},
                         .kind = dnd::TokenKind::kPlayer,
                         .cell = CellAt(2, 2)},
              dnd::Token{.id = 3,
                         .name = "lucky",
                         .owner = "carol",
                         .hp = 0,
                         .max_hp = 10,
                         .conditions = {dnd::Condition{"без сознания"}},
                         .kind = dnd::TokenKind::kPlayer,
                         .cell = CellAt(3, 3)},
          },
      .next_token_id = 4,
  };
  dnd::GameState state(snapshot);

  (void)state.DeathSave(1, 5);
  (void)state.DeathSave(1, 6);
  const std::optional<dnd::Token> dead = state.DeathSave(1, 7);
  Expect(dead.has_value() && dead->death_save_failures == 3,
         "three failures fill the death save counter");
  if (dead.has_value()) {
    Expect(std::ranges::any_of(dead->conditions,
                               [](const dnd::Condition& condition) -> bool {
                                 return condition.name == dnd::kDead;
                               }),
           "three failures kill the character");
  }

  (void)state.DeathSave(2, 10);
  (void)state.DeathSave(2, 11);
  const std::optional<dnd::Token> stable = state.DeathSave(2, 12);
  Expect(stable.has_value() && stable->death_save_successes == 3,
         "three successes stabilize the character");
  if (stable.has_value()) {
    Expect(std::ranges::none_of(stable->conditions,
                                [](const dnd::Condition& condition) -> bool {
                                  return condition.name == dnd::kUnconscious;
                                }),
           "a stable character is no longer unconscious");
  }

  const std::optional<dnd::Token> revived = state.DeathSave(3, 20);
  Expect(revived.has_value() && revived->hp == 1,
         "a natural twenty brings the character back with 1 HP");

  const std::optional<dnd::Token> hit = state.DamageToken(2, 5);
  Expect(hit.has_value() && hit->death_save_failures == 1,
         "damage at zero hp adds a death save failure");
}

auto TestConditionDurationAndInitiative() -> void {
  const dnd::GridMap map(8, 8);
  const dnd::GameSnapshot snapshot{
      .map = map,
      .tokens =
          {
              dnd::Token{.id = 1,
                         .name = "hero",
                         .owner = "alice",
                         .initiative = 20,
                         .initiative_bonus = 5,
                         .kind = dnd::TokenKind::kPlayer,
                         .cell = CellAt(1, 1)},
              dnd::Token{.id = 2,
                         .name = "goblin",
                         .owner = "dm",
                         .initiative = 10,
                         .kind = dnd::TokenKind::kMonster,
                         .cell = CellAt(2, 2)},
          },
      .next_token_id = 3,
  };

  dnd::GameServer server(snapshot, 6, 1234);
  const dnd::CommandOutcome start =
      server.HandleCommand(dnd::StartEncounter{}, "", true);
  Expect(start.accepted, "the DM starts the encounter");
  for (const dnd::Token& token : server.State().Tokens()) {
    Expect(token.initiative >= 1 + token.initiative_bonus &&
               token.initiative <= 20 + token.initiative_bonus,
           "initiative is rolled with the sheet bonus");
  }
  Expect(std::ranges::any_of(start.events,
                             [](const dnd::Event& event) -> bool {
                               return std::holds_alternative<dnd::LogLineAdded>(
                                   event);
                             }),
         "initiative rolls are written to the log");

  const dnd::CommandOutcome timed = server.HandleCommand(
      dnd::UpdateTokenCombat{.id = 1,
                             .conditions = {dnd::Condition{"отравлен", 1},
                                            dnd::Condition{"без сознания"}}},
      "alice", false);
  Expect(timed.accepted, "the owner sets a timed condition");
  Expect(TokenOf(server.State(), 1).conditions.size() == 2,
         "timed and permanent conditions coexist");

  // Полный круг: ход гоблина, затем снова ход героя — у состояния кончается
  // срок, а бессрочное «без сознания» остаётся.
  Expect(server.HandleCommand(dnd::NextTurn{}, "", true).accepted,
         "the DM advances the turn");
  Expect(server.HandleCommand(dnd::NextTurn{}, "", true).accepted,
         "the DM advances the turn again");
  const dnd::Token ticked = TokenOf(server.State(), 1);
  Expect(ticked.conditions.size() == 1 &&
             ticked.conditions.front().name == "без сознания",
         "timed condition expires at the start of the token turn");
}

auto TestDiceExpression() -> void {
  dnd::DiceExpression expression;
  Expect(dnd::ParseDiceExpression("d20+5", expression), "d20+5 is parsed");
  Expect(expression.count == 1 && expression.sides == 20 &&
             expression.modifier == 5,
         "d20+5 fields are correct");
  Expect(dnd::ParseDiceExpression("2d6-1", expression) &&
             expression.count == 2 && expression.sides == 6 &&
             expression.modifier == -1,
         "2d6-1 is parsed");
  Expect(dnd::ParseDiceExpression("adv d20+5", expression) &&
             expression.advantage && !expression.disadvantage,
         "advantage is parsed");
  Expect(dnd::ParseDiceExpression("DIS d20", expression) &&
             expression.disadvantage,
         "disadvantage is case-insensitive");

  Expect(!dnd::ParseDiceExpression("", expression), "empty expression fails");
  Expect(!dnd::ParseDiceExpression("adv 2d6", expression),
         "advantage requires d20");
  Expect(!dnd::ParseDiceExpression("d1", expression), "one-sided dice fail");
  Expect(!dnd::ParseDiceExpression("0d6", expression), "zero dice fail");
  Expect(!dnd::ParseDiceExpression("21d6", expression), "too many dice fail");
  Expect(!dnd::ParseDiceExpression("d20+101", expression),
         "too large modifier fails");

  Expect(dnd::IsDiceExpressionPrefix(""), "empty text is a valid prefix");
  Expect(dnd::IsDiceExpressionPrefix("adv "), "adv prefix is valid");
  Expect(dnd::IsDiceExpressionPrefix("d20+"), "incomplete modifier is valid");
  Expect(dnd::IsDiceExpressionPrefix("2d6+3"),
         "complete expression is a valid prefix");
  Expect(!dnd::IsDiceExpressionPrefix("x"), "letters are rejected");
  Expect(!dnd::IsDiceExpressionPrefix("advx"), "adv without space fails");
  Expect(!dnd::IsDiceExpressionPrefix("d101"),
         "too many sides is rejected while typing");
  Expect(!dnd::IsDiceExpressionPrefix("d20+101"),
         "too large modifier is rejected while typing");

  // Фиксированное зерно: тест проверяет диапазоны и арифметику, а не
  // случайность как таковую.
  // NOLINTNEXTLINE(bugprone-random-generator-seed)
  std::mt19937 rng(7);
  const dnd::DiceRollResult normal = dnd::RollDiceExpression(
      dnd::DiceExpression{.count = 2, .sides = 6, .modifier = 3}, rng);
  Expect(normal.rolls.size() == 2, "2d6 rolls two dice");
  Expect(normal.rolls[0] >= 1 && normal.rolls[0] <= 6,
         "die result is in range");
  Expect(normal.total == normal.rolls[0] + normal.rolls[1] + 3,
         "modifier is added to the total");
  Expect(!normal.breakdown.empty(), "roll has a breakdown");

  const dnd::DiceRollResult advantage = dnd::RollDiceExpression(
      dnd::DiceExpression{.sides = 20, .modifier = 1, .advantage = true}, rng);
  Expect(advantage.rolls.size() == 2, "advantage rolls two d20");
  Expect(advantage.chosen == std::max(advantage.rolls[0], advantage.rolls[1]),
         "advantage keeps the higher roll");
  Expect(advantage.total == advantage.chosen + 1,
         "advantage adds the modifier");
}

auto TestBestiary() -> void {
  const auto& bestiary = dnd::DefaultBestiary();
  Expect(!bestiary.empty(), "bestiary is not empty");
  if (!bestiary.empty()) {
    Expect(!bestiary.front().name.empty(), "bestiary entries have names");
    Expect(bestiary.front().ac > 0, "у записей бестиария есть КД");
    Expect(bestiary.front().max_hp > 0, "bestiary entries have HP");
    Expect(!bestiary.front().damage.empty(),
           "bestiary entries have damage expressions");
  }
  Expect(dnd::DefaultConditions().size() >= 8,
         "default conditions are available");
}

auto TestDynamicFogObservers() -> void {
  dnd::GridMap map(12, 1);
  map.SetBlocked(CellAt(5, 0), true);
  const dnd::GameSnapshot snapshot{
      .map = map,
      .tokens = {dnd::Token{.id = 1,
                            .name = "hero",
                            .kind = dnd::TokenKind::kPlayer,
                            .cell = CellAt(0, 0)}},
      .next_token_id = 2,
  };
  dnd::GameServer server(snapshot, 2, 2);
  Expect(!FogVisibleAt(server.FogState(), 10, 0),
         "far cells start outside party vision");

  const dnd::CommandOutcome added =
      server.HandleCommand(dnd::AddToken{.name = "ally",
                                         .cell = CellAt(10, 0),
                                         .kind = dnd::TokenKind::kPlayer,
                                         .max_hp = 10},
                           "", true);
  Expect(added.accepted, "the DM can add a player character");
  Expect(added.events.size() == 2, "adding a player refreshes the party view");
  Expect(FogVisibleAt(server.FogState(), 10, 0),
         "a new player character expands the party vision");

  const dnd::CommandOutcome removed =
      server.HandleCommand(dnd::RemoveToken{.id = 2}, "", true);
  Expect(removed.accepted, "the DM can remove the character");
  Expect(removed.events.size() == 2,
         "removing a player refreshes the party view");
  Expect(!FogVisibleAt(server.FogState(), 10, 0),
         "removing a player character removes its vision");
}

auto TestUtf8() -> void {
  const std::string text = "Привет";
  Expect(dnd::PreviousCodePointStart(text) == text.size() - 2,
         "previous code point starts before the last Cyrillic letter");
  std::string shortened = text;
  shortened.resize(dnd::PreviousCodePointStart(shortened));
  Expect(shortened == "Приве",
         "removing the last code point keeps valid UTF-8");

  std::string truncated = text;
  dnd::TruncateUtf8(truncated, 5);
  Expect(truncated == "Пр", "truncation never splits a UTF-8 sequence");
  truncated = text;
  dnd::TruncateUtf8(truncated, 6);
  Expect(truncated == "При", "truncation keeps complete characters");
  Expect(dnd::PreviousCodePointStart("") == 0, "empty text has no code point");
}

auto TestTokenManagement() -> void {
  const dnd::GridMap map(20, 20);
  const dnd::GameSnapshot snapshot{
      .map = map,
      .tokens = {dnd::Token{.id = 1,
                            .name = "hero",
                            .owner = "alice",
                            .kind = dnd::TokenKind::kPlayer,
                            .cell = CellAt(1, 1)}},
      .next_token_id = 2,
  };
  dnd::GameServer server(snapshot, 4, 3);

  const dnd::CommandOutcome player_add = server.HandleCommand(
      dnd::AddToken{.name = "orc", .cell = CellAt(2, 2)}, "alice", false);
  Expect(!player_add.accepted, "players cannot spawn tokens");

  const dnd::CommandOutcome dm_add = server.HandleCommand(
      dnd::AddToken{.name = "orc", .cell = CellAt(1, 2)}, "", true);
  Expect(dm_add.accepted, "DM can spawn tokens");
  Expect(dm_add.events.size() == 1 &&
             std::holds_alternative<dnd::TokenAdded>(dm_add.events.front()),
         "spawn yields TokenAdded");
  Expect(server.State().Tokens().size() == 2, "spawned token is in the world");
  Expect(server.EventVisibleToPlayers(dm_add.events.front()),
         "nearby spawn is visible to players");
  dnd::TokenId spawned = 0;
  if (!dm_add.events.empty() &&
      std::holds_alternative<dnd::TokenAdded>(dm_add.events.front())) {
    spawned = std::get<dnd::TokenAdded>(dm_add.events.front()).token.id;
  }
  if (spawned != 0) {
    Expect(!server
                .HandleCommand(
                    dnd::MoveToken{.id = spawned, .target = CellAt(1, 3)},
                    "alice", false)
                .accepted,
           "spawned monsters are DM-controlled");
  }

  const dnd::CommandOutcome hidden_add = server.HandleCommand(
      dnd::AddToken{.name = "far", .cell = CellAt(15, 15)}, "", true);
  Expect(hidden_add.accepted, "DM can spawn far away");
  Expect(!hidden_add.events.empty() &&
             !server.EventVisibleToPlayers(hidden_add.events.front()),
         "far spawn is hidden from players");

  const dnd::CommandOutcome off_map = server.HandleCommand(
      dnd::AddToken{.name = "void", .cell = CellAt(2, 40)}, "", true);
  Expect(!off_map.accepted, "spawn outside the map is rejected");

  const dnd::CommandOutcome hp_owner =
      server.HandleCommand(dnd::SetTokenHp{.id = 1, .hp = 5}, "alice", false);
  Expect(hp_owner.accepted, "owner can change hp");
  Expect(!server.HandleCommand(dnd::SetTokenHp{.id = 1, .hp = 3}, "bob", false)
              .accepted,
         "other players cannot change hp");

  const dnd::CommandOutcome hp_clamp =
      server.HandleCommand(dnd::SetTokenHp{.id = 1, .hp = 999}, "", true);
  Expect(hp_clamp.accepted, "DM hp update is accepted");
  for (const dnd::Token& token : server.State().Tokens()) {
    if (token.id == 1) {
      Expect(token.hp == token.max_hp, "hp is clamped to the maximum");
    }
  }

  const dnd::CommandOutcome init = server.HandleCommand(
      dnd::SetInitiative{.id = 1, .value = 18}, "alice", false);
  Expect(init.accepted, "owner can change initiative");
  for (const dnd::Token& token : server.State().Tokens()) {
    if (token.id == 1) {
      Expect(token.initiative == 18, "initiative is stored");
    }
  }

  dnd::ClientView view;
  view.Reset(server.ClientSnapshot(), server.FogState());
  for (const dnd::Event& event : dm_add.events) {
    view.Apply(event);
  }
  Expect(view.Tokens().size() == 2, "client view learns about the spawn");

  for (const dnd::Event& event : hp_owner.events) {
    view.Apply(event);
  }
  for (const dnd::Token& token : view.Tokens()) {
    if (token.id == 1) {
      Expect(token.hp == 5, "client view applies hp updates");
    }
  }

  const dnd::CommandOutcome removed =
      server.HandleCommand(dnd::RemoveToken{.id = spawned}, "", true);
  Expect(removed.accepted, "DM can remove tokens");
  for (const dnd::Event& event : removed.events) {
    view.Apply(event);
  }
  Expect(view.Tokens().size() == 1, "client view drops removed tokens");
  Expect(
      !server.HandleCommand(dnd::RemoveToken{.id = 1}, "alice", false).accepted,
      "players cannot remove tokens");
}

auto TestEncounter() -> void {
  const dnd::GridMap map(10, 10);
  dnd::GameState state(map);
  const dnd::TokenId a = state.AddToken("a", CellAt(0, 0));
  const dnd::TokenId b = state.AddToken("b", CellAt(1, 0));
  const dnd::TokenId c = state.AddToken("c", CellAt(2, 0));
  (void)state.SetInitiative(a, 5);
  (void)state.SetInitiative(b, 15);
  (void)state.SetInitiative(c, 10);

  Expect(state.StartEncounter(), "encounter starts");
  Expect(state.EncounterState().active && state.EncounterState().round == 1,
         "encounter begins in round one");
  Expect(state.EncounterState().order.size() == 3 &&
             state.EncounterState().order[0] == b &&
             state.EncounterState().order[1] == c &&
             state.EncounterState().order[2] == a,
         "order is sorted by initiative");
  Expect(state.ActiveTokenId() == b, "first turn goes to the highest roll");

  Expect(state.NextTurn() && state.ActiveTokenId() == c, "second turn");
  Expect(state.NextTurn() && state.ActiveTokenId() == a, "third turn");
  Expect(state.NextTurn() && state.EncounterState().round == 2 &&
             state.ActiveTokenId() == b,
         "wrap advances the round");

  (void)state.SetInitiative(a, 99);
  Expect(state.EncounterState().order[0] == a,
         "changing initiative re-sorts the order");
  Expect(state.ActiveTokenId() == b, "re-sorting keeps the current turn");

  state.RemoveToken(c);
  Expect(state.EncounterState().order.size() == 2,
         "removed tokens leave the order");

  state.EndEncounter();
  Expect(!state.EncounterState().active, "encounter can be ended");
  Expect(!state.NextTurn(), "next turn needs an active encounter");

  dnd::GameState empty(map);
  Expect(!empty.StartEncounter(), "empty encounter is rejected");
}

auto TestEncounterCommands() -> void {
  const dnd::GridMap map(10, 10);
  const dnd::GameSnapshot snapshot{
      .map = map,
      .tokens =
          {
              dnd::Token{.id = 1,
                         .name = "hero",
                         .owner = "alice",
                         .initiative = 5,
                         .kind = dnd::TokenKind::kPlayer,
                         .cell = CellAt(1, 1)},
              dnd::Token{.id = 2,
                         .name = "goblin",
                         .owner = "dm",
                         .initiative = 15,
                         .kind = dnd::TokenKind::kMonster,
                         .cell = CellAt(2, 2)},
          },
      .next_token_id = 3,
  };
  dnd::GameServer server(snapshot, 6, 1);

  Expect(!server.HandleCommand(dnd::StartEncounter{}, "alice", false).accepted,
         "players cannot start an encounter");
  const dnd::CommandOutcome start =
      server.HandleCommand(dnd::StartEncounter{}, "", true);
  Expect(start.accepted, "DM starts the encounter");
  Expect(!start.events.empty() && std::holds_alternative<dnd::EncounterUpdated>(
                                      start.events.front()),
         "start broadcasts EncounterUpdated");
  if (!start.events.empty() &&
      std::holds_alternative<dnd::EncounterUpdated>(start.events.front())) {
    const auto& encounter =
        std::get<dnd::EncounterUpdated>(start.events.front()).encounter;
    Expect(encounter.active && encounter.order.front() == 2,
           "goblin acts first by initiative");
    Expect(server.EventVisibleToPlayers(start.events.front()),
           "encounter updates reach players");
  }

  dnd::ClientView view;
  for (const dnd::Event& event : start.events) {
    view.Apply(event);
  }
  Expect(
      view.EncounterState().active && view.EncounterState().order.front() == 2,
      "client view tracks the encounter");

  Expect(!server.HandleCommand(dnd::NextTurn{}, "alice", false).accepted,
         "players cannot advance turns");
  const dnd::CommandOutcome next =
      server.HandleCommand(dnd::NextTurn{}, "", true);
  Expect(next.accepted, "DM advances the turn");
  Expect(server.State().EncounterState().turn_index == 1, "turn pointer moved");

  const dnd::CommandOutcome end =
      server.HandleCommand(dnd::EndEncounter{}, "", true);
  Expect(end.accepted, "DM ends the encounter");
  Expect(!server.State().EncounterState().active, "encounter is cleared");
}

auto TestMovementBudget() -> void {
  const dnd::GridMap map(12, 3);
  const dnd::GameSnapshot snapshot{
      .map = map,
      .tokens =
          {
              dnd::Token{.id = 1,
                         .name = "hero",
                         .owner = "alice",
                         .initiative = 15,
                         .speed = 9,
                         .kind = dnd::TokenKind::kPlayer,
                         .cell = CellAt(1, 1)},
          },
      .next_token_id = 2,
  };
  dnd::GameServer server(snapshot, 6, 1);
  Expect(server.HandleCommand(dnd::StartEncounter{}, "", true).accepted,
         "DM starts the encounter for movement");

  const dnd::CommandOutcome full = server.HandleCommand(
      dnd::MoveToken{.id = 1, .target = CellAt(7, 1)}, "alice", false);
  Expect(full.accepted, "player spends the whole turn of movement");
  const std::optional<dnd::Token> after_full = server.State().TokenCopy(1);
  Expect(after_full.has_value() && after_full->movement_used == 6,
         "six cells are charged for the move");

  bool movement_broadcast = false;
  for (const dnd::Event& event : full.events) {
    if (std::holds_alternative<dnd::TokenUpdated>(event)) {
      movement_broadcast =
          std::get<dnd::TokenUpdated>(event).token.movement_used == 6;
    }
  }
  Expect(movement_broadcast, "spent movement travels with TokenUpdated");

  dnd::ClientView view;
  for (const dnd::Event& event : full.events) {
    view.Apply(event);
  }
  Expect(view.Tokens().size() == 1 &&
             view.Tokens().front().cell == CellAt(7, 1) &&
             view.Tokens().front().movement_used == 6,
         "client view tracks the spent movement");

  Expect(!server
              .HandleCommand(dnd::MoveToken{.id = 1, .target = CellAt(7, 1)},
                             "alice", false)
              .accepted,
         "standing still is not a move");
  Expect(!server
              .HandleCommand(dnd::MoveToken{.id = 1, .target = CellAt(8, 1)},
                             "alice", false)
              .accepted,
         "player cannot move past the movement budget");

  Expect(server
             .HandleCommand(dnd::MoveToken{.id = 1, .target = CellAt(8, 1)}, "",
                            true)
             .accepted,
         "DM ignores the movement budget");
  const std::optional<dnd::Token> after_dm = server.State().TokenCopy(1);
  Expect(after_dm.has_value() && after_dm->movement_used == 6,
         "DM moves do not spend movement");

  Expect(server.HandleCommand(dnd::EndEncounter{}, "", true).accepted,
         "DM ends the encounter");
  const std::optional<dnd::Token> after_end = server.State().TokenCopy(1);
  Expect(after_end.has_value() && after_end->movement_used == 0,
         "ending the encounter clears spent movement");
  Expect(server
             .HandleCommand(dnd::MoveToken{.id = 1, .target = CellAt(11, 1)},
                            "alice", false)
             .accepted,
         "movement is unlimited out of combat");

  Expect(server.HandleCommand(dnd::StartEncounter{}, "", true).accepted,
         "DM restarts the encounter");
  Expect(server
             .HandleCommand(dnd::MoveToken{.id = 1, .target = CellAt(10, 1)},
                            "alice", false)
             .accepted,
         "restarting the encounter restores the budget");
  const dnd::CommandOutcome turn =
      server.HandleCommand(dnd::NextTurn{}, "", true);
  Expect(turn.accepted, "DM advances the turn");
  bool reset_broadcast = false;
  for (const dnd::Event& event : turn.events) {
    if (std::holds_alternative<dnd::TokenUpdated>(event)) {
      reset_broadcast =
          std::get<dnd::TokenUpdated>(event).token.movement_used == 0;
    }
  }
  Expect(reset_broadcast, "a new turn broadcasts the reset movement");
  const std::optional<dnd::Token> after_turn = server.State().TokenCopy(1);
  Expect(after_turn.has_value() && after_turn->movement_used == 0,
         "a new turn restores the spent movement");
  Expect(server
             .HandleCommand(dnd::MoveToken{.id = 1, .target = CellAt(4, 1)},
                            "alice", false)
             .accepted,
         "the new turn allows a fresh move");

  // Клетка внутри глухой стены недостижима, хотя сама она свободна.
  dnd::GridMap walled(8, 8);
  for (int i = 3; i <= 5; ++i) {
    walled.SetBlocked(CellAt(i, 3), true);
    walled.SetBlocked(CellAt(i, 5), true);
    walled.SetBlocked(CellAt(3, i), true);
    walled.SetBlocked(CellAt(5, i), true);
  }
  const dnd::GameSnapshot walled_snapshot{
      .map = walled,
      .tokens =
          {
              dnd::Token{.id = 1,
                         .name = "hero",
                         .owner = "alice",
                         .initiative = 15,
                         .speed = 9,
                         .kind = dnd::TokenKind::kPlayer,
                         .cell = CellAt(1, 1)},
          },
      .next_token_id = 2,
  };
  dnd::GameServer walled_server(walled_snapshot, 6, 1);
  Expect(walled_server.HandleCommand(dnd::StartEncounter{}, "", true).accepted,
         "DM starts the encounter in the walled map");
  Expect(!walled_server
              .HandleCommand(dnd::MoveToken{.id = 1, .target = CellAt(4, 4)},
                             "alice", false)
              .accepted,
         "player cannot jump over a sealed wall");
}

auto TestAoe() -> void {
  const dnd::GridMap map(10, 10);

  const dnd::AoeTemplate circle{
      .shape = dnd::AoeShape::kCircle,
      .origin = CellAt(4, 4),
      .direction = 0,
      .radius = 2,
      .width = 1,
  };
  const std::vector<dnd::Cell> circle_cells = dnd::AoeCells(map, circle);
  Expect(circle_cells.size() == 13, "circle covers thirteen cells");
  Expect(HasCell(circle_cells, CellAt(4, 4)) &&
             HasCell(circle_cells, CellAt(6, 4)) &&
             HasCell(circle_cells, CellAt(4, 2)),
         "circle reaches its radius");
  Expect(!HasCell(circle_cells, CellAt(7, 4)),
         "circle stops outside the radius");

  dnd::AoeTemplate cone = circle;
  cone.shape = dnd::AoeShape::kCone;
  const std::vector<dnd::Cell> cone_cells = dnd::AoeCells(map, cone);
  Expect(!HasCell(cone_cells, CellAt(4, 4)),
         "cone starts in front of the origin");
  Expect(HasCell(cone_cells, CellAt(5, 4)) &&
             HasCell(cone_cells, CellAt(6, 5)) &&
             HasCell(cone_cells, CellAt(6, 3)),
         "cone widens along its direction");
  Expect(!HasCell(cone_cells, CellAt(3, 4)),
         "cone does not spread behind the origin");

  dnd::AoeTemplate line = circle;
  line.shape = dnd::AoeShape::kLine;
  line.width = 1;
  const std::vector<dnd::Cell> line_cells = dnd::AoeCells(map, line);
  Expect(line_cells.size() == 3 && HasCell(line_cells, CellAt(4, 4)) &&
             HasCell(line_cells, CellAt(6, 4)) &&
             !HasCell(line_cells, CellAt(5, 5)),
         "narrow line follows one row");
  line.width = 3;
  const std::vector<dnd::Cell> wide_line = dnd::AoeCells(map, line);
  Expect(wide_line.size() == 9 && HasCell(wide_line, CellAt(5, 3)) &&
             HasCell(wide_line, CellAt(5, 5)),
         "wide line covers three rows");

  line.radius = 1;
  line.width = 7;
  const std::vector<dnd::Cell> short_wide_line = dnd::AoeCells(map, line);
  Expect(short_wide_line.size() == 14 &&
             HasCell(short_wide_line, CellAt(4, 1)) &&
             HasCell(short_wide_line, CellAt(5, 7)),
         "wide line is not clipped by a short radius");

  Expect(dnd::RotateAoe(circle, -1).direction == 7, "rotation wraps backwards");
  Expect(dnd::RotateAoe(circle, 9).direction == 1, "rotation wraps forwards");
  Expect(dnd::ResizeAoe(circle, 100).radius == dnd::kMaxAoeRadius,
         "radius is clamped at the top");
  Expect(dnd::ResizeAoe(circle, -100).radius == 1,
         "radius is clamped at the bottom");
  Expect(dnd::WidenAoe(circle, 1).width == 3, "line widens by two");
  Expect(dnd::WidenAoe(circle, -1).width == 1,
         "line width never drops below one");
  dnd::AoeTemplate max_width = circle;
  max_width.width = dnd::kMaxAoeWidth;
  Expect(dnd::WidenAoe(max_width, 1).width % 2 == 1 &&
             dnd::WidenAoe(max_width, 1).width <= dnd::kMaxAoeWidth,
         "line width stays odd and in range");
  Expect(dnd::NextAoeShape(dnd::AoeShape::kCircle) == dnd::AoeShape::kCone &&
             dnd::NextAoeShape(dnd::AoeShape::kCone) == dnd::AoeShape::kLine &&
             dnd::NextAoeShape(dnd::AoeShape::kLine) == dnd::AoeShape::kCircle,
         "shape cycles through all kinds");

  dnd::AoeTemplate invalid = circle;
  invalid.width = 2;
  Expect(!dnd::IsValidAoe(invalid) && dnd::AoeCells(map, invalid).empty(),
         "even width is rejected");
  invalid = circle;
  invalid.direction = dnd::kAoeDirections;
  Expect(!dnd::IsValidAoe(invalid), "direction outside the range is rejected");
  invalid = circle;
  invalid.radius = 0;
  Expect(!dnd::IsValidAoe(invalid), "zero radius is rejected");
  invalid = circle;
  invalid.origin = CellAt(20, 20);
  Expect(dnd::AoeCells(map, invalid).empty(),
         "template outside the map has no cells");
  Expect(dnd::AoeCells(map, dnd::AoeTemplate{}).empty(),
         "hidden template has no cells");
}

auto TestAoeCommands() -> void {
  const dnd::GridMap map(10, 10);
  const dnd::GameSnapshot snapshot{
      .map = map,
      .tokens = {dnd::Token{.id = 1,
                            .name = "hero",
                            .owner = "alice",
                            .kind = dnd::TokenKind::kPlayer,
                            .cell = CellAt(1, 1)}},
      .next_token_id = 2,
  };
  dnd::GameServer server(snapshot, 6, 1);

  const dnd::SetAoe set{.aoe = dnd::AoeTemplate{
                            .shape = dnd::AoeShape::kCircle,
                            .origin = CellAt(3, 3),
                            .direction = 0,
                            .radius = 3,
                            .width = 1,
                        }};
  Expect(!server.HandleCommand(set, "alice", false).accepted,
         "players cannot place a template");

  const dnd::CommandOutcome placed = server.HandleCommand(set, "", true);
  Expect(placed.accepted, "DM places a template");
  Expect(placed.events.size() == 1 &&
             std::holds_alternative<dnd::AoeUpdated>(placed.events.front()),
         "placing broadcasts AoeUpdated");
  if (!placed.events.empty() &&
      std::holds_alternative<dnd::AoeUpdated>(placed.events.front())) {
    const auto& aoe = std::get<dnd::AoeUpdated>(placed.events.front()).aoe;
    Expect(aoe == set.aoe, "event carries the template");
    Expect(server.EventVisibleToPlayers(placed.events.front()),
           "templates are visible to players");
  }
  Expect(server.State().Aoe() == set.aoe, "server keeps the template");
  Expect(server.ClientSnapshot().aoe == set.aoe,
         "template is part of the snapshot");

  dnd::ClientView view;
  for (const dnd::Event& event : placed.events) {
    view.Apply(event);
  }
  Expect(view.Aoe() == set.aoe, "client view tracks the template");

  dnd::SetAoe even_width = set;
  even_width.aoe.width = 2;
  Expect(!server.HandleCommand(even_width, "", true).accepted,
         "even line width is rejected by the server");
  dnd::SetAoe outside = set;
  outside.aoe.origin = CellAt(50, 50);
  Expect(!server.HandleCommand(outside, "", true).accepted,
         "template outside the map is rejected by the server");

  const dnd::CommandOutcome cleared =
      server.HandleCommand(dnd::SetAoe{}, "", true);
  Expect(cleared.accepted, "DM clears the template");
  Expect(server.State().Aoe().shape == dnd::AoeShape::kNone,
         "clearing hides the template");
  if (!cleared.events.empty() &&
      std::holds_alternative<dnd::AoeUpdated>(cleared.events.front())) {
    view.Apply(cleared.events.front());
  }
  Expect(view.Aoe().shape == dnd::AoeShape::kNone,
         "client view forgets the template");
}

auto TestSaveLoadRoundtrip() -> void {
  dnd::GridMap map(6, 4);
  map.SetBlocked(CellAt(2, 1), true);
  const dnd::GameSnapshot original{
      .map = map,
      .tokens =
          {
              dnd::Token{.id = 4,
                         .name = "hero",
                         .owner = "alice",
                         .hp = 7,
                         .max_hp = 12,
                         .temp_hp = 3,
                         .initiative = 15,
                         .ac = 16,
                         .speed = 9,
                         .notes = "Атака: +5, 1d8+3",
                         .attack_bonus = 5,
                         .save_bonus = 2,
                         .damage = "1d8+3",
                         .conditions = {dnd::Condition{"сбит с ног"},
                                        dnd::Condition{"концентрация"}},
                         .movement_used = 4,
                         .kind = dnd::TokenKind::kPlayer,
                         .color = 0x507AC8U,
                         .cell = CellAt(1, 1)},
              dnd::Token{.id = 6,
                         .name = "goblin",
                         .owner = "dm",
                         .kind = dnd::TokenKind::kMonster,
                         .cell = CellAt(3, 2)},
          },
      .next_token_id = 7,
      .encounter =
          dnd::Encounter{
              .active = true,
              .round = 3,
              .order = {4, 6},
              .turn_index = 1,
          },
      .aoe =
          dnd::AoeTemplate{
              .shape = dnd::AoeShape::kCone,
              .origin = CellAt(2, 2),
              .direction = 5,
              .radius = 4,
              .width = 3,
          },
  };

  const std::filesystem::path path =
      std::filesystem::temp_directory_path() / "dnd_core_tests_save.bin";

  Expect(dnd::WriteSnapshot(original, path), "snapshot writes");
  const std::optional<dnd::GameSnapshot> loaded = dnd::ReadSnapshot(path);
  Expect(loaded.has_value(), "snapshot reads back");
  if (loaded.has_value()) {
    Expect(loaded->map.Width() == 6 && loaded->map.Height() == 4,
           "dimensions survive the roundtrip");
    Expect(loaded->map.IsBlocked(CellAt(2, 1)), "walls survive the roundtrip");
    Expect(loaded->tokens.size() == 2, "token count survives the roundtrip");
    Expect(loaded->tokens.front().name == "hero", "token name survives");
    Expect(loaded->tokens.front().owner == "alice", "token owner survives");
    Expect(loaded->tokens.front().hp == 7, "token hp survives");
    Expect(loaded->tokens.front().max_hp == 12, "token max hp survives");
    Expect(loaded->tokens.front().temp_hp == 3, "token temporary hp survives");
    Expect(loaded->tokens.front().initiative == 15,
           "token initiative survives");
    Expect(loaded->tokens.front().ac == 16, "token AC survives");
    Expect(loaded->tokens.front().speed == 9, "token speed survives");
    Expect(loaded->tokens.front().notes == "Атака: +5, 1d8+3",
           "token notes survive");
    Expect(loaded->tokens.front().attack_bonus == 5,
           "token attack bonus survives");
    Expect(loaded->tokens.front().save_bonus == 2, "token save bonus survives");
    Expect(loaded->tokens.front().damage == "1d8+3",
           "token damage expression survives");
    Expect(loaded->tokens.front().conditions.size() == 2,
           "token conditions survive");
    Expect(loaded->tokens.front().movement_used == 4,
           "token spent movement survives");
    Expect(loaded->tokens.front().kind == dnd::TokenKind::kPlayer,
           "token kind survives");
    Expect(loaded->tokens.front().color == 0x507AC8U, "token colour survives");
    Expect(loaded->tokens.front().cell == CellAt(1, 1), "token cell survives");
    Expect(loaded->next_token_id == 7, "next token id survives");
    Expect(loaded->encounter.active && loaded->encounter.round == 3,
           "encounter survives the roundtrip");
    Expect(loaded->encounter.order == std::vector<dnd::TokenId>{4, 6} &&
               loaded->encounter.turn_index == 1,
           "turn order survives the roundtrip");
    Expect(loaded->aoe == original.aoe, "aoe template survives the roundtrip");
  }

  std::filesystem::remove(path);
  Expect(!dnd::ReadSnapshot(path).has_value(), "missing file is rejected");

  const std::filesystem::path bad_path =
      std::filesystem::temp_directory_path() / "dnd_core_tests_bad.bin";
  {
    std::ofstream out(bad_path, std::ios::binary | std::ios::trunc);
    out.write("nope", 4);
  }
  Expect(!dnd::ReadSnapshot(bad_path).has_value(), "bad magic is rejected");
  std::filesystem::remove(bad_path);
}

auto TestCampaign() -> void {
  dnd::GridMap first_map(5, 4);
  first_map.SetBlocked(CellAt(1, 1), true);
  const dnd::GameSnapshot first{
      .map = first_map,
      .tokens =
          {
              dnd::Token{.id = 1,
                         .name = "hero",
                         .kind = dnd::TokenKind::kPlayer,
                         .cell = CellAt(0, 0)},
          },
      .next_token_id = 2,
  };
  const dnd::GameSnapshot second{
      .map = dnd::GridMap(3, 3),
      .next_token_id = 1,
  };

  dnd::Campaign campaign = dnd::MakeCampaign(first, "");
  Expect(
      campaign.scenes.size() == 1 && campaign.scenes.front().name == "Сцена 1",
      "empty scene name gets a default");
  Expect(campaign.active == 0, "campaign starts on its only scene");
  Expect(dnd::NextSceneName(campaign) == "Сцена 2",
         "next scene name avoids used ones");

  campaign.scenes.push_back(dnd::Scene{.name = "Сцена 2", .snapshot = second});
  campaign.active = 1;
  Expect(dnd::NextSceneName(campaign) == "Сцена 3",
         "next scene name counts existing scenes");
  Expect(!dnd::IsSceneNameAvailable(campaign, "Сцена 1"),
         "an existing scene name is unavailable");
  Expect(dnd::IsSceneNameAvailable(campaign, "Сцена 1", 0),
         "a scene may keep its own name");
  Expect(!dnd::IsSceneNameAvailable(campaign, ""),
         "an empty scene name is rejected");
  Expect(!dnd::RenameScene(campaign, 1, "Сцена 1"),
         "rename rejects a duplicate name");
  Expect(dnd::RenameScene(campaign, 1, "Подземелье"),
         "rename accepts a free name");
  const std::optional<dnd::SceneRenameRequest> spaced =
      dnd::ParseSceneRename(campaign, "Сцена 1 Пещера");
  Expect(
      spaced.has_value() && spaced->index == 0 && spaced->new_name == "Пещера",
      "scene rename parses names with spaces");
  const std::optional<dnd::SceneRenameRequest> plain =
      dnd::ParseSceneRename(campaign, "Подземелье Логово");
  Expect(plain.has_value() && plain->index == 1 && plain->new_name == "Логово",
         "scene rename parses a one-word scene name");
  Expect(!dnd::ParseSceneRename(campaign, "Нет такой").has_value(),
         "scene rename rejects an unknown scene");
  Expect(!dnd::ParseSceneRename(campaign, "Сцена 1").has_value(),
         "scene rename needs a new name");
  campaign.scenes.front().note = "Герои спустились в подземелье";

  const std::filesystem::path path =
      std::filesystem::temp_directory_path() / "dnd_core_tests_campaign.bin";
  Expect(dnd::WriteCampaign(campaign, path), "campaign writes");
  const std::optional<dnd::Campaign> loaded = dnd::ReadCampaign(path);
  Expect(loaded.has_value(), "campaign reads back");
  if (loaded.has_value()) {
    Expect(loaded->scenes.size() == 2, "scene count survives the roundtrip");
    Expect(loaded->active == 1, "active scene survives the roundtrip");
    Expect(loaded->scenes.front().name == "Сцена 1" &&
               loaded->scenes.back().name == "Подземелье",
           "scene names survive the roundtrip");
    Expect(loaded->scenes.front().note == "Герои спустились в подземелье",
           "scene notes survive the roundtrip");
    Expect(loaded->scenes.front().snapshot.map.Width() == 5 &&
               loaded->scenes.front().snapshot.map.IsBlocked(CellAt(1, 1)),
           "scene walls survive the roundtrip");
    Expect(loaded->scenes.front().snapshot.tokens.size() == 1 &&
               loaded->scenes.front().snapshot.tokens.front().name == "hero",
           "scene tokens survive the roundtrip");
    Expect(loaded->scenes.back().snapshot.map.Width() == 3,
           "second scene keeps its own map");
  }
  std::filesystem::remove(path);
  Expect(!dnd::ReadCampaign(path).has_value(), "missing campaign is rejected");

  const std::filesystem::path bad_path =
      std::filesystem::temp_directory_path() /
      "dnd_core_tests_campaign_bad.bin";
  {
    std::ofstream out(bad_path, std::ios::binary | std::ios::trunc);
    out.write("DNDC", 4);
  }
  Expect(!dnd::ReadCampaign(bad_path).has_value(),
         "truncated campaign is rejected");
  std::filesystem::remove(bad_path);
}

// Испорченные байты — обычное дело при обрыве связи или битом файле.
// Декодеры должны отвечать отказом, а не падать и не читать за границей
// буфера; случайно принятые данные обязаны переживать повторное кодирование.
auto TestDamagedPayloads() -> void {
  dnd::GridMap map(6, 4);
  map.SetBlocked(CellAt(2, 2), true);
  const dnd::GameSnapshot snapshot{
      .map = map,
      .tokens = {dnd::Token{.id = 1,
                            .name = "hero",
                            .kind = dnd::TokenKind::kPlayer,
                            .cell = CellAt(1, 1)}},
      .next_token_id = 2,
  };
  const std::vector<std::uint8_t> snapshot_bytes =
      dnd::EncodeSnapshot(snapshot);
  const std::vector<std::uint8_t> command_bytes = dnd::EncodeCommand(
      dnd::Command{dnd::MoveToken{.id = 1, .target = CellAt(2, 1)}});
  const std::vector<std::uint8_t> event_bytes =
      dnd::EncodeEvent(dnd::Event{dnd::TokenRemoved{.id = 1}});

  // Зерно фиксировано, чтобы поломка воспроизводилась.
  // NOLINTNEXTLINE(bugprone-random-generator-seed)
  std::mt19937 rng(2024);
  const auto mutate =
      [&rng](std::vector<std::uint8_t> bytes) -> std::vector<std::uint8_t> {
    const int edits = 1 + static_cast<int>(rng() % 4);
    for (int i = 0; i < edits && !bytes.empty(); ++i) {
      bytes[rng() % bytes.size()] = static_cast<std::uint8_t>(rng());
    }
    if (rng() % 2 == 0) {
      bytes.resize(rng() % (bytes.size() + 1));
    }
    return bytes;
  };

  std::size_t accepted_snapshots = 0;
  for (int round = 0; round < 300; ++round) {
    const std::vector<std::uint8_t> damaged = mutate(snapshot_bytes);
    const std::optional<dnd::GameSnapshot> decoded =
        dnd::DecodeSnapshot(damaged.data(), damaged.size());
    if (decoded.has_value()) {
      ++accepted_snapshots;
      const std::vector<std::uint8_t> again = dnd::EncodeSnapshot(*decoded);
      Expect(dnd::DecodeSnapshot(again.data(), again.size()).has_value(),
             "accepted snapshot survives re-encoding");
    }

    const std::vector<std::uint8_t> broken_command = mutate(command_bytes);
    if (const std::optional<dnd::Command> command =
            dnd::DecodeCommand(broken_command.data(), broken_command.size())) {
      const std::vector<std::uint8_t> again = dnd::EncodeCommand(*command);
      Expect(dnd::DecodeCommand(again.data(), again.size()).has_value(),
             "accepted command survives re-encoding");
    }

    const std::vector<std::uint8_t> broken_event = mutate(event_bytes);
    if (const std::optional<dnd::Event> event =
            dnd::DecodeEvent(broken_event.data(), broken_event.size())) {
      const std::vector<std::uint8_t> again = dnd::EncodeEvent(*event);
      Expect(dnd::DecodeEvent(again.data(), again.size()).has_value(),
             "accepted event survives re-encoding");
    }
  }
  Expect(accepted_snapshots < 300,
         "damaged snapshots are almost always rejected");

  const std::filesystem::path path =
      std::filesystem::temp_directory_path() / "dnd_core_tests_garbage.bin";
  for (int round = 0; round < 50; ++round) {
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(rng() % 96));
    for (std::uint8_t& value : bytes) {
      value = static_cast<std::uint8_t>(rng());
    }
    {
      std::ofstream out(path, std::ios::binary | std::ios::trunc);
      out.write(reinterpret_cast<const char*>(bytes.data()),
                static_cast<std::streamsize>(bytes.size()));
    }
    Expect(!dnd::ReadCampaign(path).has_value(),
           "garbage campaign file is rejected");
  }
  std::filesystem::remove(path);
}

auto TestChatAndDice() -> void {
  const dnd::GridMap map(10, 10);
  const dnd::GameSnapshot snapshot{
      .map = map,
      .tokens = {dnd::Token{.id = 1,
                            .name = "hero",
                            .kind = dnd::TokenKind::kPlayer,
                            .cell = CellAt(1, 1)}},
      .next_token_id = 2,
  };
  dnd::GameServer server(snapshot, 6, 7);

  const dnd::CommandOutcome say =
      server.HandleCommand(dnd::Say{.text = "hello"}, "alice", false);
  Expect(say.accepted, "chat is accepted");
  Expect(say.events.size() == 1 &&
             std::holds_alternative<dnd::LogLineAdded>(say.events.front()),
         "chat produces a log line");
  if (!say.events.empty() &&
      std::holds_alternative<dnd::LogLineAdded>(say.events.front())) {
    const auto& line = std::get<dnd::LogLineAdded>(say.events.front()).line;
    Expect(line.author == "alice", "chat keeps the author");
    Expect(line.text == "hello", "chat keeps the text");
    Expect(line.timestamp != 0, "chat carries a timestamp");
  }

  const dnd::CommandOutcome roll =
      server.HandleCommand(dnd::RollDice{.count = 2, .sides = 6}, "", true);
  Expect(roll.accepted, "dice roll is accepted");
  if (!roll.events.empty() &&
      std::holds_alternative<dnd::LogLineAdded>(roll.events.front())) {
    const auto& line = std::get<dnd::LogLineAdded>(roll.events.front()).line;
    Expect(line.author == "DM", "DM rolls are signed as DM");
    Expect(line.text.find("2d6") != std::string::npos,
           "dice line mentions the notation");
    Expect(line.text.find('=') != std::string::npos,
           "dice line contains the total");
    Expect(line.timestamp != 0, "dice roll carries a timestamp");
  }

  const dnd::CommandOutcome expression_roll = server.HandleCommand(
      dnd::RollExpression{.expression = "adv d20+5", .label = "Атака"}, "alice",
      false);
  Expect(expression_roll.accepted, "roll expression is accepted");
  if (!expression_roll.events.empty() &&
      std::holds_alternative<dnd::LogLineAdded>(
          expression_roll.events.front())) {
    const auto& line =
        std::get<dnd::LogLineAdded>(expression_roll.events.front()).line;
    Expect(line.text.find("Атака") != std::string::npos,
           "roll label is included");
    Expect(line.text.find("adv d20+5") != std::string::npos,
           "roll expression is included");
  }
  Expect(!server
              .HandleCommand(dnd::RollExpression{.expression = "adv 2d6"},
                             "alice", false)
              .accepted,
         "invalid advantage expression is rejected");
  Expect(!server
              .HandleCommand(dnd::RollExpression{.expression = "nonsense"},
                             "alice", false)
              .accepted,
         "invalid expression is rejected");

  Expect(
      !server
           .HandleCommand(dnd::RollDice{.count = 0, .sides = 6}, "alice", false)
           .accepted,
      "zero dice are rejected");
  Expect(
      !server
           .HandleCommand(dnd::RollDice{.count = 1, .sides = 1}, "alice", false)
           .accepted,
      "one-sided dice are rejected");
  Expect(!server.HandleCommand(dnd::Say{.text = ""}, "alice", false).accepted,
         "empty chat is rejected");

  dnd::ClientView view;
  for (const dnd::Event& event : say.events) {
    view.Apply(event);
  }
  Expect(view.Log().size() == 1, "client view stores log lines");
  Expect(view.AppendedLogLines() == 1, "client view counts stored lines");
  view.Apply(dnd::ViewUpdated{
      .snapshot = server.ClientSnapshot(),
      .fog = server.FogState(),
  });
  Expect(view.Log().size() == 1, "view refresh keeps the log history");

  view.AddLocalLine("local only");
  Expect(view.Log().size() == 2 && view.Log().back().text == "local only" &&
             view.Log().back().author.empty() &&
             view.Log().back().timestamp != 0,
         "local feedback stays in the client log");
  Expect(view.AppendedLogLines() == 2,
         "local feedback is counted for the session log");
}

auto TestSessionLog() -> void {
  const dnd::LogLine chat{
      .author = "alice", .text = "привет", .timestamp = 1763650000};
  const std::string formatted = dnd::FormatSessionLogLine(chat);
  Expect(formatted.size() > 19 && formatted.front() == '[' &&
             formatted.find("] alice: привет\n") != std::string::npos,
         "session log line carries time, author and text");
  Expect(dnd::FormatSessionLogLine(dnd::LogLine{.text = "без времени"}) ==
             "без времени\n",
         "session log line without time stays plain");

  const std::filesystem::path path =
      std::filesystem::temp_directory_path() / "dnd_core_tests_session.log";
  std::filesystem::remove(path);
  {
    dnd::SessionLog log;
    Expect(log.Open(path.string(), 1763650000), "session log opens");
    Expect(log.IsOpen(), "session log reports the open file");
    log.Append(chat);
    log.Append(dnd::LogLine{.text = "вторая строка"});
    log.Flush();
    log.Close();
    Expect(!log.IsOpen(), "session log closes the file");
  }
  {
    // Повторный запуск дописывает, а не стирает прошлую сессию.
    dnd::SessionLog log;
    Expect(log.Open(path.string(), 1763651000), "session log reopens");
    log.Append(chat);
    log.Close();
  }
  std::ifstream in(path, std::ios::binary);
  const std::string content{std::istreambuf_iterator<char>(in),
                            std::istreambuf_iterator<char>()};
  Expect(content.find("=== сессия ") != std::string::npos,
         "session log marks its start");
  Expect(content.find("вторая строка\n") != std::string::npos,
         "session log stores system lines");
  std::size_t author_hits = 0;
  for (std::size_t at = content.find("alice: привет"); at != std::string::npos;
       at = content.find("alice: привет", at + 1)) {
    ++author_hits;
  }
  Expect(author_hits == 2, "session log appends to the previous file");
  in.close();
  std::filesystem::remove(path);

  dnd::SessionLog broken;
  Expect(!broken.Open((std::filesystem::temp_directory_path() /
                       "dnd_core_tests_no_such_dir" / "session.log")
                          .string(),
                      1763650000) &&
             !broken.IsOpen(),
         "unwritable session log reports failure");
}

auto TestPing() -> void {
  const dnd::GridMap map(10, 10);
  const dnd::GameSnapshot snapshot{.map = map, .next_token_id = 1};
  dnd::GameServer server(snapshot, 6, 7);

  const dnd::CommandOutcome ping =
      server.HandleCommand(dnd::PingAt{.cell = CellAt(3, 4)}, "alice", false);
  Expect(ping.accepted && ping.events.size() == 1 &&
             std::holds_alternative<dnd::PingMarker>(ping.events.front()),
         "ping is accepted and becomes a marker event");
  if (!ping.events.empty() &&
      std::holds_alternative<dnd::PingMarker>(ping.events.front())) {
    const auto& marker = std::get<dnd::PingMarker>(ping.events.front());
    Expect(marker.cell == CellAt(3, 4) && marker.author == "alice",
           "ping keeps the cell and the author");
    Expect(server.EventVisibleToPlayers(ping.events.front()),
           "ping is visible to every player");
  }
  Expect(
      !server.HandleCommand(dnd::PingAt{.cell = CellAt(10, 0)}, "alice", false)
           .accepted,
      "off-map ping is rejected");

  dnd::ClientView view;
  for (const dnd::Event& event : ping.events) {
    view.Apply(event);
  }
  Expect(view.Pings().size() == 1 &&
             view.Pings().front().cell == CellAt(3, 4) &&
             view.Pings().front().author == "alice",
         "client view stores the ping");
  const std::uint32_t created = view.Pings().front().created_at;
  view.PrunePings(created + dnd::kPingSeconds - 1);
  Expect(view.Pings().size() == 1, "fresh ping survives pruning");
  view.PrunePings(created + dnd::kPingSeconds);
  Expect(view.Pings().empty(), "expired ping is pruned");

  view.Apply(dnd::PingMarker{.cell = CellAt(2, 2), .author = "bob"});
  view.Apply(dnd::ViewUpdated{.snapshot = server.ClientSnapshot(),
                              .fog = server.FogState()});
  Expect(view.Pings().empty(), "world refresh clears pings");
}

auto TestCommandEventSerialization() -> void {
  const dnd::Command say{dnd::Say{.text = "hi there"}};
  const std::vector<std::uint8_t> say_bytes = dnd::EncodeCommand(say);
  const std::optional<dnd::Command> say_decoded =
      dnd::DecodeCommand(say_bytes.data(), say_bytes.size());
  Expect(say_decoded.has_value() &&
             std::holds_alternative<dnd::Say>(*say_decoded) &&
             std::get<dnd::Say>(*say_decoded).text == "hi there",
         "say survives serialization");

  const dnd::Command roll{dnd::RollDice{.count = 3, .sides = 8}};
  const std::vector<std::uint8_t> roll_bytes = dnd::EncodeCommand(roll);
  const std::optional<dnd::Command> roll_decoded =
      dnd::DecodeCommand(roll_bytes.data(), roll_bytes.size());
  Expect(roll_decoded.has_value() &&
             std::holds_alternative<dnd::RollDice>(*roll_decoded) &&
             std::get<dnd::RollDice>(*roll_decoded).count == 3 &&
             std::get<dnd::RollDice>(*roll_decoded).sides == 8,
         "roll survives serialization");

  const dnd::Command expression_command{
      dnd::RollExpression{.expression = "adv d20+5", .label = "Атака"}};
  const std::vector<std::uint8_t> expression_bytes =
      dnd::EncodeCommand(expression_command);
  const std::optional<dnd::Command> expression_decoded =
      dnd::DecodeCommand(expression_bytes.data(), expression_bytes.size());
  Expect(
      expression_decoded.has_value() &&
          std::holds_alternative<dnd::RollExpression>(*expression_decoded) &&
          std::get<dnd::RollExpression>(*expression_decoded).expression ==
              "adv d20+5" &&
          std::get<dnd::RollExpression>(*expression_decoded).label == "Атака",
      "roll expression survives serialization");

  const dnd::Command ping_command{dnd::PingAt{.cell = CellAt(4, 5)}};
  const std::vector<std::uint8_t> ping_bytes = dnd::EncodeCommand(ping_command);
  const std::optional<dnd::Command> ping_decoded =
      dnd::DecodeCommand(ping_bytes.data(), ping_bytes.size());
  Expect(ping_decoded.has_value() &&
             std::holds_alternative<dnd::PingAt>(*ping_decoded) &&
             std::get<dnd::PingAt>(*ping_decoded).cell == CellAt(4, 5),
         "ping command survives serialization");

  const dnd::Event marker{
      dnd::PingMarker{.cell = CellAt(4, 5), .author = "Аня"}};
  const std::vector<std::uint8_t> marker_bytes = dnd::EncodeEvent(marker);
  const std::optional<dnd::Event> marker_decoded =
      dnd::DecodeEvent(marker_bytes.data(), marker_bytes.size());
  Expect(marker_decoded.has_value() &&
             std::holds_alternative<dnd::PingMarker>(*marker_decoded) &&
             std::get<dnd::PingMarker>(*marker_decoded).cell == CellAt(4, 5) &&
             std::get<dnd::PingMarker>(*marker_decoded).author == "Аня",
         "ping marker survives serialization");

  const dnd::Event line{dnd::LogLineAdded{
      .line =
          dnd::LogLine{.author = "bob", .text = "oops", .timestamp = 123456},
  }};
  const std::vector<std::uint8_t> line_bytes = dnd::EncodeEvent(line);
  const std::optional<dnd::Event> line_decoded =
      dnd::DecodeEvent(line_bytes.data(), line_bytes.size());
  Expect(
      line_decoded.has_value() &&
          std::holds_alternative<dnd::LogLineAdded>(*line_decoded) &&
          std::get<dnd::LogLineAdded>(*line_decoded).line.author == "bob" &&
          std::get<dnd::LogLineAdded>(*line_decoded).line.timestamp == 123456,
      "log line survives serialization");

  const dnd::Command add{dnd::AddToken{
      .name = "orc",
      .cell = CellAt(5, 6),
      .owner = "dm",
      .kind = dnd::TokenKind::kMonster,
      .max_hp = 13,
      .ac = 14,
      .speed = 12,
      .notes = "Дубина: +4, 1d8+2",
      .attack_bonus = 4,
      .save_bonus = 1,
      .damage = "1d8+2",
  }};
  const std::vector<std::uint8_t> add_bytes = dnd::EncodeCommand(add);
  const std::optional<dnd::Command> add_decoded =
      dnd::DecodeCommand(add_bytes.data(), add_bytes.size());
  Expect(
      add_decoded.has_value() &&
          std::holds_alternative<dnd::AddToken>(*add_decoded) &&
          std::get<dnd::AddToken>(*add_decoded).name == "orc" &&
          std::get<dnd::AddToken>(*add_decoded).cell == CellAt(5, 6) &&
          std::get<dnd::AddToken>(*add_decoded).owner == "dm" &&
          std::get<dnd::AddToken>(*add_decoded).kind ==
              dnd::TokenKind::kMonster &&
          std::get<dnd::AddToken>(*add_decoded).max_hp == 13 &&
          std::get<dnd::AddToken>(*add_decoded).ac == 14 &&
          std::get<dnd::AddToken>(*add_decoded).speed == 12 &&
          std::get<dnd::AddToken>(*add_decoded).notes == "Дубина: +4, 1d8+2" &&
          std::get<dnd::AddToken>(*add_decoded).attack_bonus == 4 &&
          std::get<dnd::AddToken>(*add_decoded).save_bonus == 1 &&
          std::get<dnd::AddToken>(*add_decoded).damage == "1d8+2",
      "spawn command survives serialization");

  const dnd::Command claim{dnd::ClaimToken{.id = 4}};
  const std::vector<std::uint8_t> claim_bytes = dnd::EncodeCommand(claim);
  const std::optional<dnd::Command> claim_decoded =
      dnd::DecodeCommand(claim_bytes.data(), claim_bytes.size());
  Expect(claim_decoded.has_value() &&
             std::holds_alternative<dnd::ClaimToken>(*claim_decoded) &&
             std::get<dnd::ClaimToken>(*claim_decoded).id == 4,
         "claim command survives serialization");

  const dnd::Command release{dnd::ReleaseToken{.id = 4}};
  const std::vector<std::uint8_t> release_bytes = dnd::EncodeCommand(release);
  const std::optional<dnd::Command> release_decoded =
      dnd::DecodeCommand(release_bytes.data(), release_bytes.size());
  Expect(release_decoded.has_value() &&
             std::holds_alternative<dnd::ReleaseToken>(*release_decoded) &&
             std::get<dnd::ReleaseToken>(*release_decoded).id == 4,
         "release command survives serialization");

  const dnd::Command owner{dnd::SetTokenOwner{.id = 4, .owner = "alice"}};
  const std::vector<std::uint8_t> owner_bytes = dnd::EncodeCommand(owner);
  const std::optional<dnd::Command> owner_decoded =
      dnd::DecodeCommand(owner_bytes.data(), owner_bytes.size());
  Expect(owner_decoded.has_value() &&
             std::holds_alternative<dnd::SetTokenOwner>(*owner_decoded) &&
             std::get<dnd::SetTokenOwner>(*owner_decoded).owner == "alice",
         "owner command survives serialization");

  const dnd::Command rename{dnd::RenameToken{.id = 4, .name = "Алиса"}};
  const std::vector<std::uint8_t> rename_bytes = dnd::EncodeCommand(rename);
  const std::optional<dnd::Command> rename_decoded =
      dnd::DecodeCommand(rename_bytes.data(), rename_bytes.size());
  Expect(rename_decoded.has_value() &&
             std::holds_alternative<dnd::RenameToken>(*rename_decoded) &&
             std::get<dnd::RenameToken>(*rename_decoded).name == "Алиса",
         "rename command survives serialization");

  const dnd::Command max_hp{dnd::SetTokenMaxHp{.id = 4, .max_hp = 22}};
  const std::vector<std::uint8_t> max_hp_bytes = dnd::EncodeCommand(max_hp);
  const std::optional<dnd::Command> max_hp_decoded =
      dnd::DecodeCommand(max_hp_bytes.data(), max_hp_bytes.size());
  Expect(max_hp_decoded.has_value() &&
             std::holds_alternative<dnd::SetTokenMaxHp>(*max_hp_decoded) &&
             std::get<dnd::SetTokenMaxHp>(*max_hp_decoded).max_hp == 22,
         "max hp command survives serialization");

  const dnd::Command color{dnd::SetTokenColor{.id = 4, .color = 0xC85046U}};
  const std::vector<std::uint8_t> color_bytes = dnd::EncodeCommand(color);
  const std::optional<dnd::Command> color_decoded =
      dnd::DecodeCommand(color_bytes.data(), color_bytes.size());
  Expect(color_decoded.has_value() &&
             std::holds_alternative<dnd::SetTokenColor>(*color_decoded) &&
             std::get<dnd::SetTokenColor>(*color_decoded).color == 0xC85046U,
         "colour command survives serialization");

  const dnd::Command sheet_command{
      dnd::UpdateTokenSheet{.id = 4,
                            .ac = 15,
                            .speed = 9,
                            .notes = "Атака: +4, 1d6+2",
                            .attack_bonus = 4,
                            .save_bonus = 1,
                            .initiative_bonus = 3,
                            .damage = "1d6+2",
                            .temp_hp = 5}};
  const std::vector<std::uint8_t> sheet_bytes =
      dnd::EncodeCommand(sheet_command);
  const std::optional<dnd::Command> sheet_decoded =
      dnd::DecodeCommand(sheet_bytes.data(), sheet_bytes.size());
  Expect(
      sheet_decoded.has_value() &&
          std::holds_alternative<dnd::UpdateTokenSheet>(*sheet_decoded) &&
          std::get<dnd::UpdateTokenSheet>(*sheet_decoded).ac == 15 &&
          std::get<dnd::UpdateTokenSheet>(*sheet_decoded).speed == 9 &&
          std::get<dnd::UpdateTokenSheet>(*sheet_decoded).notes ==
              "Атака: +4, 1d6+2" &&
          std::get<dnd::UpdateTokenSheet>(*sheet_decoded).attack_bonus == 4 &&
          std::get<dnd::UpdateTokenSheet>(*sheet_decoded).save_bonus == 1 &&
          std::get<dnd::UpdateTokenSheet>(*sheet_decoded).initiative_bonus ==
              3 &&
          std::get<dnd::UpdateTokenSheet>(*sheet_decoded).damage == "1d6+2" &&
          std::get<dnd::UpdateTokenSheet>(*sheet_decoded).temp_hp == 5,
      "sheet command survives serialization");

  const dnd::Command combat_command{
      dnd::UpdateTokenCombat{.id = 4,
                             .conditions = {dnd::Condition{"сбит с ног"},
                                            dnd::Condition{"отравлен"}}}};
  const std::vector<std::uint8_t> combat_bytes =
      dnd::EncodeCommand(combat_command);
  const std::optional<dnd::Command> combat_decoded =
      dnd::DecodeCommand(combat_bytes.data(), combat_bytes.size());
  Expect(
      combat_decoded.has_value() &&
          std::holds_alternative<dnd::UpdateTokenCombat>(*combat_decoded) &&
          std::get<dnd::UpdateTokenCombat>(*combat_decoded).conditions.size() ==
              2,
      "combat command survives serialization");

  const dnd::Command damage{dnd::DamageToken{.id = 4, .amount = 7}};
  const std::vector<std::uint8_t> damage_bytes = dnd::EncodeCommand(damage);
  const std::optional<dnd::Command> damage_decoded =
      dnd::DecodeCommand(damage_bytes.data(), damage_bytes.size());
  Expect(damage_decoded.has_value() &&
             std::holds_alternative<dnd::DamageToken>(*damage_decoded) &&
             std::get<dnd::DamageToken>(*damage_decoded).id == 4 &&
             std::get<dnd::DamageToken>(*damage_decoded).amount == 7,
         "damage command survives serialization");

  const dnd::Command heal{dnd::HealToken{.id = 5, .amount = 3}};
  const std::vector<std::uint8_t> heal_bytes = dnd::EncodeCommand(heal);
  const std::optional<dnd::Command> heal_decoded =
      dnd::DecodeCommand(heal_bytes.data(), heal_bytes.size());
  Expect(heal_decoded.has_value() &&
             std::holds_alternative<dnd::HealToken>(*heal_decoded) &&
             std::get<dnd::HealToken>(*heal_decoded).id == 5 &&
             std::get<dnd::HealToken>(*heal_decoded).amount == 3,
         "heal command survives serialization");

  const dnd::Command aoe_damage{
      dnd::AoeDamage{.expression = "2d6", .save_dc = 15}};
  const std::vector<std::uint8_t> aoe_damage_bytes =
      dnd::EncodeCommand(aoe_damage);
  const std::optional<dnd::Command> aoe_damage_decoded =
      dnd::DecodeCommand(aoe_damage_bytes.data(), aoe_damage_bytes.size());
  Expect(
      aoe_damage_decoded.has_value() &&
          std::holds_alternative<dnd::AoeDamage>(*aoe_damage_decoded) &&
          std::get<dnd::AoeDamage>(*aoe_damage_decoded).expression == "2d6" &&
          std::get<dnd::AoeDamage>(*aoe_damage_decoded).save_dc == 15,
      "aoe damage command survives serialization");

  const dnd::Command death_save{dnd::DeathSave{.id = 6}};
  const std::vector<std::uint8_t> death_save_bytes =
      dnd::EncodeCommand(death_save);
  const std::optional<dnd::Command> death_save_decoded =
      dnd::DecodeCommand(death_save_bytes.data(), death_save_bytes.size());
  Expect(death_save_decoded.has_value() &&
             std::holds_alternative<dnd::DeathSave>(*death_save_decoded) &&
             std::get<dnd::DeathSave>(*death_save_decoded).id == 6,
         "death save command survives serialization");

  const dnd::Command rest{dnd::RestParty{.long_rest = true}};
  const std::vector<std::uint8_t> rest_bytes = dnd::EncodeCommand(rest);
  const std::optional<dnd::Command> rest_decoded =
      dnd::DecodeCommand(rest_bytes.data(), rest_bytes.size());
  Expect(rest_decoded.has_value() &&
             std::holds_alternative<dnd::RestParty>(*rest_decoded) &&
             std::get<dnd::RestParty>(*rest_decoded).long_rest,
         "rest command survives serialization");

  const dnd::Event updated{dnd::TokenUpdated{
      .token = dnd::Token{.id = 3,
                          .name = "orc",
                          .owner = "dm",
                          .hp = 2,
                          .max_hp = 9,
                          .temp_hp = 3,
                          .initiative = 11,
                          .ac = 14,
                          .speed = 12,
                          .notes = "Дубина: +4, 1d8+2",
                          .attack_bonus = 4,
                          .save_bonus = 1,
                          .damage = "1d8+2",
                          .conditions = {dnd::Condition{"оглушён", 2}},
                          .movement_used = 5,
                          .death_save_successes = 1,
                          .death_save_failures = 2,
                          .kind = dnd::TokenKind::kMonster,
                          .color = 0x46A864U,
                          .cell = CellAt(1, 1)},
  }};
  const std::vector<std::uint8_t> updated_bytes = dnd::EncodeEvent(updated);
  const std::optional<dnd::Event> updated_decoded =
      dnd::DecodeEvent(updated_bytes.data(), updated_bytes.size());
  Expect(
      updated_decoded.has_value() &&
          std::holds_alternative<dnd::TokenUpdated>(*updated_decoded) &&
          std::get<dnd::TokenUpdated>(*updated_decoded).token.hp == 2 &&
          std::get<dnd::TokenUpdated>(*updated_decoded).token.initiative ==
              11 &&
          std::get<dnd::TokenUpdated>(*updated_decoded).token.ac == 14 &&
          std::get<dnd::TokenUpdated>(*updated_decoded).token.speed == 12 &&
          std::get<dnd::TokenUpdated>(*updated_decoded).token.notes ==
              "Дубина: +4, 1d8+2" &&
          std::get<dnd::TokenUpdated>(*updated_decoded).token.attack_bonus ==
              4 &&
          std::get<dnd::TokenUpdated>(*updated_decoded).token.save_bonus == 1 &&
          std::get<dnd::TokenUpdated>(*updated_decoded).token.damage ==
              "1d8+2" &&
          std::get<dnd::TokenUpdated>(*updated_decoded).token.temp_hp == 3 &&
          std::get<dnd::TokenUpdated>(*updated_decoded)
                  .token.conditions.size() == 1 &&
          std::get<dnd::TokenUpdated>(*updated_decoded)
                  .token.conditions.front()
                  .rounds == 2 &&
          std::get<dnd::TokenUpdated>(*updated_decoded).token.movement_used ==
              5 &&
          std::get<dnd::TokenUpdated>(*updated_decoded)
                  .token.death_save_successes == 1 &&
          std::get<dnd::TokenUpdated>(*updated_decoded)
                  .token.death_save_failures == 2 &&
          std::get<dnd::TokenUpdated>(*updated_decoded).token.kind ==
              dnd::TokenKind::kMonster &&
          std::get<dnd::TokenUpdated>(*updated_decoded).token.color ==
              0x46A864U,
      "token update survives serialization");

  const dnd::Command start{dnd::StartEncounter{}};
  const std::vector<std::uint8_t> start_bytes = dnd::EncodeCommand(start);
  const std::optional<dnd::Command> start_decoded =
      dnd::DecodeCommand(start_bytes.data(), start_bytes.size());
  Expect(start_decoded.has_value() &&
             std::holds_alternative<dnd::StartEncounter>(*start_decoded),
         "encounter command survives serialization");

  const dnd::Event encounter{dnd::EncounterUpdated{
      .encounter =
          dnd::Encounter{
              .active = true,
              .round = 2,
              .order = {1, 2},
              .turn_index = 1,
          },
  }};
  const std::vector<std::uint8_t> encounter_bytes = dnd::EncodeEvent(encounter);
  const std::optional<dnd::Event> encounter_decoded =
      dnd::DecodeEvent(encounter_bytes.data(), encounter_bytes.size());
  Expect(
      encounter_decoded.has_value() &&
          std::holds_alternative<dnd::EncounterUpdated>(*encounter_decoded) &&
          std::get<dnd::EncounterUpdated>(*encounter_decoded).encounter.round ==
              2 &&
          std::get<dnd::EncounterUpdated>(*encounter_decoded)
                  .encounter.turn_index == 1,
      "encounter event survives serialization");

  const dnd::Command place_aoe{dnd::SetAoe{.aoe = dnd::AoeTemplate{
                                               .shape = dnd::AoeShape::kLine,
                                               .origin = CellAt(3, 5),
                                               .direction = 2,
                                               .radius = 6,
                                               .width = 5,
                                           }}};
  const std::vector<std::uint8_t> place_aoe_bytes =
      dnd::EncodeCommand(place_aoe);
  const std::optional<dnd::Command> place_aoe_decoded =
      dnd::DecodeCommand(place_aoe_bytes.data(), place_aoe_bytes.size());
  Expect(place_aoe_decoded.has_value() &&
             std::holds_alternative<dnd::SetAoe>(*place_aoe_decoded) &&
             std::get<dnd::SetAoe>(*place_aoe_decoded).aoe ==
                 std::get<dnd::SetAoe>(place_aoe).aoe,
         "aoe command survives serialization");

  const dnd::Event aoe_event{dnd::AoeUpdated{
      .aoe =
          dnd::AoeTemplate{
              .shape = dnd::AoeShape::kCircle,
              .origin = CellAt(1, 7),
              .direction = 7,
              .radius = 2,
              .width = 1,
          },
  }};
  const std::vector<std::uint8_t> aoe_event_bytes = dnd::EncodeEvent(aoe_event);
  const std::optional<dnd::Event> aoe_event_decoded =
      dnd::DecodeEvent(aoe_event_bytes.data(), aoe_event_bytes.size());
  Expect(aoe_event_decoded.has_value() &&
             std::holds_alternative<dnd::AoeUpdated>(*aoe_event_decoded) &&
             std::get<dnd::AoeUpdated>(*aoe_event_decoded).aoe ==
                 std::get<dnd::AoeUpdated>(aoe_event).aoe,
         "aoe event survives serialization");

  const dnd::Command clear_aoe{dnd::SetAoe{}};
  const std::vector<std::uint8_t> clear_aoe_bytes =
      dnd::EncodeCommand(clear_aoe);
  const std::optional<dnd::Command> clear_aoe_decoded =
      dnd::DecodeCommand(clear_aoe_bytes.data(), clear_aoe_bytes.size());
  Expect(clear_aoe_decoded.has_value() &&
             std::holds_alternative<dnd::SetAoe>(*clear_aoe_decoded) &&
             std::get<dnd::SetAoe>(*clear_aoe_decoded).aoe.shape ==
                 dnd::AoeShape::kNone,
         "clearing the template survives serialization");

  std::vector<std::uint8_t> even_width_bytes = place_aoe_bytes;
  constexpr std::size_t width_offset = 1 + 1 + 4 + 4 + 4 + 4;
  even_width_bytes[width_offset] = 4;
  Expect(!dnd::DecodeCommand(even_width_bytes.data(), even_width_bytes.size())
              .has_value(),
         "decoder rejects an even line width");
}

auto HexDigest(const dnd::Sha256Digest& digest) -> std::string {
  constexpr std::string_view digits = "0123456789abcdef";
  std::string text;
  text.reserve(digest.size() * 2);
  for (const std::uint8_t byte : digest) {
    text.push_back(digits[byte >> 4]);
    text.push_back(digits[byte & 0x0FU]);
  }
  return text;
}

auto TestSha256() -> void {
  // Векторы из FIPS 180-4 и RFC 4231: без них ошибку в раундах не видно.
  Expect(HexDigest(dnd::Sha256("")) ==
             "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
         "sha256 hashes the empty string");
  Expect(HexDigest(dnd::Sha256("abc")) ==
             "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
         "sha256 hashes a short message");
  Expect(HexDigest(dnd::Sha256(
             "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")) ==
             "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1",
         "sha256 hashes two blocks");

  const std::string message = "The quick brown fox jumps over the lazy dog";
  const std::string key = "key";
  const auto* key_bytes = reinterpret_cast<const std::uint8_t*>(key.data());
  const auto* message_bytes =
      reinterpret_cast<const std::uint8_t*>(message.data());
  Expect(HexDigest(dnd::HmacSha256(
             std::span<const std::uint8_t>(key_bytes, key.size()),
             std::span<const std::uint8_t>(message_bytes, message.size()))) ==
             "f7bc83f430538424b13298e6aa6fb143ef4d59a14946175997479dbc2d1a3cd8",
         "hmac-sha256 matches the reference value");

  // Отпечаток пароля зависит от запроса: тот же пароль на другой nonce даёт
  // другое значение, иначе подсмотренный ответ можно переиграть.
  const dnd::ChallengeNonce first{};
  dnd::ChallengeNonce second{};
  second[0] = 1;
  const auto first_proof = dnd::MakePasswordProof("secret", first);
  Expect(
      dnd::ProofMatches(first_proof, dnd::MakePasswordProof("secret", first)),
      "proof is deterministic for the same challenge");
  Expect(
      !dnd::ProofMatches(first_proof, dnd::MakePasswordProof("secret", second)),
      "proof changes with the challenge");
  Expect(
      !dnd::ProofMatches(first_proof, dnd::MakePasswordProof("other", first)),
      "proof changes with the password");
}

}  // namespace

auto main() -> int {
  try {
    TestGridMap();
    TestMoveRules();
    TestFogLineOfSight();
    TestFogDiagonalSeal();
    TestFogExploredMemory();
    TestServerVisibility();
    TestOwnership();
    TestClaimAndTokenAdmin();
    TestDamageHealRest();
    TestConcentration();
    TestAoeDamage();
    TestDeathSaves();
    TestConditionDurationAndInitiative();
    TestDiceExpression();
    TestBestiary();
    TestDynamicFogObservers();
    TestUtf8();
    TestTokenManagement();
    TestEncounter();
    TestEncounterCommands();
    TestMovementBudget();
    TestAoe();
    TestAoeCommands();
    TestChatAndDice();
    TestPing();
    TestCommandEventSerialization();
    TestSaveLoadRoundtrip();
    TestCampaign();
    TestDamagedPayloads();
    TestSessionLog();
    TestSha256();
  } catch (const std::exception& error) {
    std::fprintf(stderr, "unexpected exception: %s\n", error.what());
    return 1;
  }

  if (g_failures == 0) {
    std::printf("all core checks passed\n");
    return 0;
  }
  std::fprintf(stderr, "%d core check(s) failed\n", g_failures);
  return 1;
}
