#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
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
#include <thread>
#include <variant>
#include <vector>

#include "core/aoe.h"
#include "core/attack.h"
#include "core/autosave.h"
#include "core/bestiary.h"
#include "core/camera.h"
#include "core/campaign.h"
#include "core/campaign_io.h"
#include "core/campaign_save.h"
#include "core/chacha20.h"
#include "core/chat_command.h"
#include "core/chat_input.h"
#include "core/client_view.h"
#include "core/conditions.h"
#include "core/delete_confirm.h"
#include "core/dice.h"
#include "core/editor_paint.h"
#include "core/editor_token.h"
#include "core/game_server.h"
#include "core/game_state.h"
#include "core/grid_map.h"
#include "core/levels.h"
#include "core/light.h"
#include "core/local_transport.h"
#include "core/log_history.h"
#include "core/log_scroll.h"
#include "core/menu_model.h"
#include "core/movement.h"
#include "core/navigation.h"
#include "core/overlay.h"
#include "core/overlay_io.h"
#include "core/overlay_save.h"
#include "core/password.h"
#include "core/player_name.h"
#include "core/scene_io.h"
#include "core/serialization.h"
#include "core/session_crypto.h"
#include "core/session_log.h"
#include "core/sha256.h"
#include "core/status_message.h"
#include "core/ui_hint.h"
#include "core/units.h"
#include "core/update.h"
#include "core/utf8.h"
#include "core/version.h"
#include "core/visibility.h"
#include "core/x25519.h"

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

using dnd::HasCondition;

auto FogVisibleAt(const dnd::FogSnapshot& fog, int x, int y) -> bool {
  return fog.visible[static_cast<std::size_t>(y) *
                         static_cast<std::size_t>(fog.width) +
                     static_cast<std::size_t>(x)] != 0;
}

auto FogExploredAt(const dnd::FogSnapshot& fog, int x, int y) -> bool {
  return fog.explored[static_cast<std::size_t>(y) *
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
  Expect(HasCell(reachable, CellAt(1, 1)),
         "first diagonal costs one cell under 5-10-5");
  Expect(!HasCell(reachable, CellAt(2, 2)), "movement range excludes walls");
  Expect(!HasCell(reachable, CellAt(3, 3)), "movement range respects budget");

  dnd::GridMap corner_map(3, 3);
  corner_map.SetBlocked(CellAt(1, 0), true);
  corner_map.SetBlocked(CellAt(0, 1), true);
  const auto corner_range = dnd::ReachableCells(corner_map, CellAt(0, 0), 2);
  Expect(!HasCell(corner_range, CellAt(1, 1)),
         "movement range does not cut through a sealed corner");

  const auto straight =
      dnd::PathCostCells(reachable_map, CellAt(0, 0), CellAt(2, 0));
  Expect(straight.has_value() && straight->cost == 2,
         "path cost counts straight steps");
  const auto stay =
      dnd::PathCostCells(reachable_map, CellAt(0, 0), CellAt(0, 0));
  Expect(stay.has_value() && stay->cost == 0 && stay->diagonal_parity == 0,
         "path cost to the same cell is zero");
  Expect(!dnd::PathCostCells(reachable_map, CellAt(0, 0), CellAt(2, 2)),
         "path cost skips walls");
  Expect(!dnd::PathCostCells(corner_map, CellAt(0, 0), CellAt(1, 1)),
         "path cost does not cut through a sealed corner");

  const dnd::GridMap open(6, 6);
  const auto two_diagonals =
      dnd::PathCostCells(open, CellAt(0, 0), CellAt(2, 2));
  Expect(two_diagonals.has_value() && two_diagonals->cost == 3 &&
             two_diagonals->diagonal_parity == 0,
         "two diagonals cost 5 then 10 feet under 5-10-5");
  const auto three_diagonals =
      dnd::PathCostCells(open, CellAt(0, 0), CellAt(3, 3));
  Expect(three_diagonals.has_value() && three_diagonals->cost == 4 &&
             three_diagonals->diagonal_parity == 1,
         "the third diagonal costs 5 feet again");
  const auto odd_start =
      dnd::PathCostCells(open, CellAt(0, 0), CellAt(1, 1), 1);
  Expect(odd_start.has_value() && odd_start->cost == 2 &&
             odd_start->diagonal_parity == 0,
         "a spent diagonal makes the next one cost 10 feet");

  const std::array<dnd::Cell, 1> blocked{CellAt(1, 0)};
  const auto detour =
      dnd::PathCostCells(open, CellAt(0, 0), CellAt(2, 0), 0, blocked);
  Expect(detour.has_value() && detour->cost > 2,
         "occupied cells force a detour");
  const auto occupied_target =
      dnd::PathCostCells(open, CellAt(0, 0), CellAt(1, 0), 0, blocked);
  Expect(!occupied_target.has_value(), "an occupied target is unreachable");

  dnd::GridMap terrain_map(4, 4);
  terrain_map.SetTerrain(CellAt(1, 0), dnd::Terrain::kTree);
  terrain_map.SetTerrain(CellAt(2, 0), dnd::Terrain::kWater);
  terrain_map.SetTerrain(CellAt(0, 2), dnd::Terrain::kPit);
  terrain_map.SetTerrain(CellAt(1, 2), dnd::Terrain::kLava);
  Expect(terrain_map.TerrainAt(CellAt(1, 0)) == dnd::Terrain::kTree,
         "terrain is stored per cell");
  Expect(!terrain_map.IsBlocked(CellAt(1, 0)) &&
             terrain_map.IsDifficultTerrain(CellAt(1, 0)),
         "a tree is passable difficult terrain");
  for (const dnd::Cell cell : {CellAt(2, 0), CellAt(1, 2)}) {
    Expect(!terrain_map.IsBlocked(cell),
           "water and lava are passable by the 2024 rules");
    Expect(!terrain_map.BlocksSight(cell),
           "water, pits and lava do not block sight");
  }
  Expect(terrain_map.IsBlocked(CellAt(0, 2)), "a pit blocks movement");
  Expect(!terrain_map.BlocksSight(CellAt(1, 0)),
         "a tree does not block sight completely");
  const auto tree_path =
      dnd::PathCostCells(terrain_map, CellAt(0, 0), CellAt(1, 0));
  Expect(tree_path.has_value() && tree_path->cost == 2,
         "entering a tree costs double movement");
  const auto water_path =
      dnd::PathCostCells(terrain_map, CellAt(1, 0), CellAt(2, 0));
  Expect(water_path.has_value() && water_path->cost == 2,
         "deep water costs double movement");
  Expect(
      !dnd::PathCostCells(terrain_map, CellAt(0, 0), CellAt(0, 2)).has_value(),
      "the pit cell itself is unreachable");

  dnd::GridMap pit_map(3, 1);
  pit_map.SetTerrain(CellAt(1, 0), dnd::Terrain::kPit);
  const auto pit_reach = dnd::ReachableCells(pit_map, CellAt(0, 0), 4);
  Expect(!HasCell(pit_reach, CellAt(1, 0)), "movement range excludes a pit");
  Expect(!HasCell(pit_reach, CellAt(2, 0)),
         "a pit cuts the map without a detour");
}

auto TestCellPropsMovement() -> void {
  // Свойства клетки поверх рельефа: трудная местность не складывается,
  // дорога её отменяет, вода и лазание стоят как в правилах 2024.
  dnd::GridMap map(4, 1);
  map.SetTerrain(CellAt(1, 0), dnd::Terrain::kSwamp);
  Expect(map.MovementCost(CellAt(1, 0), dnd::MovementProfile{}) == 2,
         "swamp is difficult terrain");
  dnd::CellProps swamp = map.PropsAt(CellAt(1, 0));
  swamp.water = dnd::WaterDepth::kShallow;
  map.SetProps(CellAt(1, 0), swamp);
  Expect(map.MovementCost(CellAt(1, 0), dnd::MovementProfile{}) == 2,
         "difficult terrain does not stack with shallow water");
  swamp.flags |= dnd::kCellRoad;
  map.SetProps(CellAt(1, 0), swamp);
  Expect(map.MovementCost(CellAt(1, 0), dnd::MovementProfile{}) == 1,
         "a road cancels difficult terrain");

  dnd::GridMap stairs(3, 3);
  dnd::CellProps climb = stairs.PropsAt(CellAt(1, 0));
  climb.flags |= dnd::kCellClimbable;
  stairs.SetProps(CellAt(1, 0), climb);
  Expect(stairs.MovementCost(CellAt(1, 0), dnd::MovementProfile{}) == 2,
         "climbing without a climb speed costs double");
  Expect(stairs.MovementCost(CellAt(1, 0),
                             dnd::MovementProfile{.climb_speed = 4}) == 1,
         "a climb speed makes the wall cost normal movement");

  dnd::GridMap pool(3, 3);
  pool.SetTerrain(CellAt(1, 1), dnd::Terrain::kWater);
  Expect(pool.MovementCost(CellAt(1, 1), dnd::MovementProfile{}) == 2,
         "deep water costs double without a swim speed");
  Expect(pool.MovementCost(CellAt(1, 1),
                           dnd::MovementProfile{.swim_speed = 4}) == 1,
         "a swim speed makes deep water cost normal movement");
  dnd::GridMap marsh(2, 2);
  marsh.SetTerrain(CellAt(1, 1), dnd::Terrain::kSwamp);
  const auto first_diagonal =
      dnd::PathCostCells(marsh, CellAt(0, 0), CellAt(1, 1));
  Expect(first_diagonal.has_value() && first_diagonal->cost == 2,
         "difficult terrain doubles the first diagonal");
}

auto TestCover() -> void {
  dnd::GridMap map(6, 1);
  dnd::CellProps middle = map.PropsAt(CellAt(2, 0));
  dnd::SetCoverOnSide(middle.cover_sides, 3, dnd::CoverLevel::kHalf);
  map.SetProps(CellAt(2, 0), middle);
  Expect(dnd::CoverOnSide(middle.cover_sides, 3) == dnd::CoverLevel::kHalf,
         "cover is stored on the west side");
  Expect(dnd::CoverBetween(map, CellAt(0, 0), CellAt(5, 0)) ==
             dnd::CoverLevel::kHalf,
         "half cover on the line of sight is found");
  Expect(dnd::CoverBonus(dnd::CoverLevel::kHalf) == 2 &&
             dnd::CoverBonus(dnd::CoverLevel::kThreeQuarters) == 5,
         "half cover gives +2 and three quarters +5 to AC");
  dnd::SetCoverOnSide(middle.cover_sides, 3, dnd::CoverLevel::kThreeQuarters);
  map.SetProps(CellAt(2, 0), middle);
  Expect(dnd::CoverBetween(map, CellAt(0, 0), CellAt(5, 0)) ==
             dnd::CoverLevel::kThreeQuarters,
         "three quarters cover upgrades the line of sight");
  // Укрытие на стороне цели, обращённой к атакующему.
  dnd::CellProps target = map.PropsAt(CellAt(5, 0));
  dnd::SetCoverOnSide(target.cover_sides, 3, dnd::CoverLevel::kFull);
  map.SetProps(CellAt(5, 0), target);
  Expect(dnd::CoverBetween(map, CellAt(0, 0), CellAt(5, 0)) ==
             dnd::CoverLevel::kFull,
         "cover on the target's facing side counts");
  map.SetBlocked(CellAt(3, 0), true);
  Expect(dnd::CoverBetween(map, CellAt(0, 0), CellAt(5, 0)) ==
             dnd::CoverLevel::kFull,
         "a wall between attacker and target is full cover");
}

auto TestHazardsAndCurrent() -> void {
  dnd::GridMap map(4, 1);
  map.SetTerrain(CellAt(1, 0), dnd::Terrain::kLava);
  std::vector<dnd::HazardProfile> hazards = dnd::DefaultHazards();
  hazards.push_back(dnd::HazardProfile{
      .damage = "1d6",
      .type = dnd::DamageType::kSlashing,
      .triggers = dnd::kHazardEnter,
      .save_dc = 10,
      .half_on_success = true,
  });
  map.SetHazards(std::move(hazards));
  dnd::CellProps spiked = map.PropsAt(CellAt(2, 0));
  spiked.hazard = 5;
  map.SetProps(CellAt(2, 0), spiked);

  const dnd::GameSnapshot snapshot{
      .map = map,
      .tokens =
          {
              dnd::Token{.id = 1,
                         .name = "hero",
                         .owner = "dm",
                         .hp = 1000,
                         .max_hp = 1000,
                         .save_bonus = 100,
                         .kind = dnd::TokenKind::kPlayer,
                         .cell = CellAt(0, 0)},
          },
      .next_token_id = 2,
  };
  dnd::GameServer server(snapshot, 6, 77);
  Expect(server.HandleCommand(dnd::StartEncounter{}, "", true).accepted,
         "the encounter starts");
  Expect(server
             .HandleCommand(dnd::MoveToken{.id = 1, .target = CellAt(1, 0)},
                            "dm", true)
             .accepted,
         "the hero steps into lava");
  Expect(TokenOf(server.State(), 1).hp < 1000,
         "lava damages on the first entry");
  const int after_lava = TokenOf(server.State(), 1).hp;
  Expect(!TokenOf(server.State(), 1).hazards_done.empty(),
         "the hazard stays marked for this turn");
  Expect(server
             .HandleCommand(dnd::MoveToken{.id = 1, .target = CellAt(0, 0)},
                            "dm", true)
             .accepted,
         "the hero leaves the lava");
  Expect(server
             .HandleCommand(dnd::MoveToken{.id = 1, .target = CellAt(1, 0)},
                            "dm", true)
             .accepted,
         "the hero returns within the same turn");
  Expect(TokenOf(server.State(), 1).hp == after_lava,
         "re-entering the same hazard in one turn does not repeat damage");
  // Спасбросок делит урон пополам: с бонусом 100 успех гарантирован.
  const int before_spikes = TokenOf(server.State(), 1).hp;
  Expect(server
             .HandleCommand(dnd::MoveToken{.id = 1, .target = CellAt(2, 0)},
                            "dm", true)
             .accepted,
         "the hero steps on spikes");
  const int spike_damage = before_spikes - TokenOf(server.State(), 1).hp;
  Expect(spike_damage >= 0 && spike_damage <= 3,
         "a successful save halves the hazard damage");
  // Новый ход снова срабатывает лавой и снимает метки прошлого хода.
  Expect(server
             .HandleCommand(dnd::MoveToken{.id = 1, .target = CellAt(1, 0)},
                            "dm", true)
             .accepted,
         "the hero stands in the lava again");
  Expect(server.HandleCommand(dnd::NextTurn{}, "", true).accepted,
         "the next turn starts");
  Expect(TokenOf(server.State(), 1).hp < after_lava,
         "turn start repeats the hazard after its mark is cleared");
}

auto TestCurrentShift() -> void {
  dnd::GridMap map(3, 1);
  dnd::CellProps flow = map.PropsAt(CellAt(1, 0));
  flow.current_dir = 1;  // восток
  flow.current_strength = 1;
  map.SetProps(CellAt(1, 0), flow);
  map.SetBlocked(CellAt(2, 0), true);
  Expect(dnd::CurrentShiftCells(1) == 1 && dnd::CurrentDc(1) == 15,
         "current strength sets the shift and the DC");

  dnd::GameSnapshot snapshot{
      .map = map,
      .tokens =
          {
              dnd::Token{.id = 1,
                         .name = "swimmer",
                         .owner = "dm",
                         .hp = 100,
                         .max_hp = 100,
                         .save_bonus = -100,
                         .kind = dnd::TokenKind::kPlayer,
                         .cell = CellAt(0, 0)},
          },
      .next_token_id = 2,
  };
  dnd::GameServer server(snapshot, 5, 31);
  Expect(server
             .HandleCommand(dnd::MoveToken{.id = 1, .target = CellAt(1, 0)},
                            "dm", true)
             .accepted,
         "the swimmer enters the current");
  Expect(TokenOf(server.State(), 1).cell == CellAt(1, 0),
         "a blocked current presses the token against the obstacle");

  dnd::GridMap open(3, 1);
  open.SetProps(CellAt(1, 0), flow);
  dnd::GameSnapshot pushed{
      .map = open,
      .tokens =
          {
              dnd::Token{.id = 1,
                         .name = "swimmer",
                         .owner = "dm",
                         .hp = 100,
                         .max_hp = 100,
                         .save_bonus = -100,
                         .kind = dnd::TokenKind::kPlayer,
                         .cell = CellAt(0, 0)},
          },
      .next_token_id = 2,
  };
  dnd::GameServer pushed_server(pushed, 5, 31);
  Expect(pushed_server
             .HandleCommand(dnd::MoveToken{.id = 1, .target = CellAt(1, 0)},
                            "dm", true)
             .accepted,
         "the swimmer enters the open current");
  Expect(TokenOf(pushed_server.State(), 1).cell == CellAt(2, 0),
         "a failed check pushes the token along the current");
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

  dnd::GridMap tree_map(6, 1);
  tree_map.SetTerrain(CellAt(2, 0), dnd::Terrain::kTree);
  dnd::FogOfWar tree_fog(6, 1);
  tree_fog.Recompute(tree_map, {CellAt(0, 0)}, 5);
  Expect(tree_fog.IsVisible(CellAt(2, 0)), "the tree cell itself is visible");
  Expect(tree_fog.IsVisible(CellAt(5, 0)),
         "a tree is light obscurement and does not shorten sight");

  dnd::GridMap water_map(6, 1);
  water_map.SetTerrain(CellAt(2, 0), dnd::Terrain::kWater);
  water_map.SetTerrain(CellAt(3, 0), dnd::Terrain::kLava);
  dnd::FogOfWar water_fog(6, 1);
  water_fog.Recompute(water_map, {CellAt(0, 0)}, 5);
  Expect(water_fog.IsVisible(CellAt(5, 0)),
         "water and lava do not block sight");

  // Сильное затемнение обрывает луч, но сама клетка видна облаком.
  dnd::GridMap smoke_map(6, 1);
  dnd::CellProps smoke = smoke_map.PropsAt(CellAt(2, 0));
  smoke.obscurement = dnd::Obscurement::kHeavy;
  smoke_map.SetProps(CellAt(2, 0), smoke);
  dnd::FogOfWar smoke_fog(6, 1);
  smoke_fog.Recompute(smoke_map, {CellAt(0, 0)}, 5);
  Expect(smoke_fog.IsVisible(CellAt(2, 0)), "the smoke cell is visible");
  Expect(!smoke_fog.IsVisible(CellAt(3, 0)), "smoke blocks sight beyond it");

  // Слабое затемнение (кустарник) обзор не режет.
  dnd::GridMap haze_map(6, 1);
  dnd::CellProps haze = haze_map.PropsAt(CellAt(2, 0));
  haze.obscurement = dnd::Obscurement::kLight;
  haze_map.SetProps(CellAt(2, 0), haze);
  dnd::FogOfWar haze_fog(6, 1);
  haze_fog.Recompute(haze_map, {CellAt(0, 0)}, 5);
  Expect(haze_fog.IsVisible(CellAt(5, 0)),
         "light obscurement does not block sight");
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

  // Точечная правка после FogChanged: ровно полученное состояние.
  fog.SetCell(CellAt(5, 0), true, true);
  Expect(fog.IsVisible(CellAt(5, 0)) && fog.IsExplored(CellAt(5, 0)),
         "a cell patch makes a cell visible and explored");
  fog.SetCell(CellAt(5, 0), false, true);
  Expect(!fog.IsVisible(CellAt(5, 0)) && fog.IsExplored(CellAt(5, 0)),
         "a cell patch can hide an explored cell");
  fog.SetCell(CellAt(99, 99), true, true);
  Expect(!fog.IsVisible(CellAt(99, 99)), "patches outside the map are ignored");
}

auto TestLightField() -> void {
  dnd::GridMap map(8, 1);
  map.SetAmbientLight(dnd::LightLevel::kDark);
  const dnd::LightField field = dnd::ComputeLightField(
      map, {dnd::LightSource{
               .cell = CellAt(0, 0), .bright_cells = 1, .dim_cells = 3}});
  Expect(field.At(CellAt(0, 0)) == dnd::LightLevel::kBright,
         "the source cell is bright");
  Expect(field.At(CellAt(1, 0)) == dnd::LightLevel::kBright,
         "the bright radius covers the next cell");
  Expect(field.At(CellAt(2, 0)) == dnd::LightLevel::kDim,
         "outside the bright radius is dim");
  Expect(field.At(CellAt(3, 0)) == dnd::LightLevel::kDim,
         "the dim radius reaches its edge");
  Expect(field.At(CellAt(4, 0)) == dnd::LightLevel::kDark,
         "beyond the dim radius is dark");

  dnd::GridMap walled(8, 1);
  walled.SetAmbientLight(dnd::LightLevel::kDark);
  walled.SetBlocked(CellAt(2, 0), true);
  const dnd::LightField wall_light = dnd::ComputeLightField(
      walled, {dnd::LightSource{
                  .cell = CellAt(0, 0), .bright_cells = 1, .dim_cells = 6}});
  Expect(wall_light.At(CellAt(2, 0)) == dnd::LightLevel::kDim,
         "the wall face itself is lit");
  Expect(wall_light.At(CellAt(3, 0)) == dnd::LightLevel::kDark,
         "cells behind a wall stay dark");

  // Кисть редактора задаёт свет клетки, пока источник его не поднимет.
  dnd::GridMap painted(3, 1);
  dnd::CellProps props = painted.PropsAt(CellAt(1, 0));
  props.light = static_cast<std::uint8_t>(dnd::LightLevel::kDim);
  painted.SetProps(CellAt(1, 0), props);
  const dnd::LightField painted_light = dnd::ComputeLightField(painted, {});
  Expect(painted_light.At(CellAt(1, 0)) == dnd::LightLevel::kDim,
         "a painted cell keeps its light");
  Expect(painted_light.At(CellAt(0, 0)) == dnd::LightLevel::kBright,
         "other cells use the ambient light");

  Expect(dnd::LightCellsFromMeters(9) == 6 && dnd::LightMetersFromCells(6) == 9,
         "meters and cells convert both ways");
}

auto TestDarkvisionAndSmoke() -> void {
  dnd::GridMap map(8, 1);
  map.SetAmbientLight(dnd::LightLevel::kDark);
  const dnd::LightField dark = dnd::ComputeLightField(map, {});
  dnd::FogOfWar fog(8, 1);
  fog.Recompute(map,
                {dnd::Observer{.cell = CellAt(0, 0), .darkvision_cells = 3}},
                dark, 0);
  Expect(fog.IsVisible(CellAt(0, 0)),
         "the observer sees its own cell in the dark");
  Expect(fog.IsVisible(CellAt(3, 0)), "darkvision reaches its radius");
  Expect(!fog.IsVisible(CellAt(4, 0)), "beyond darkvision is unseen");
  Expect(fog.LightAt(CellAt(0, 0)) == dnd::LightLevel::kDark,
         "a cell seen by darkvision stays dark for drawing");

  dnd::FogOfWar blind(8, 1);
  blind.Recompute(
      map,
      {dnd::Observer{
          .cell = CellAt(0, 0), .darkvision_cells = 6, .blinded = true}},
      dark, 0);
  Expect(!blind.IsVisible(CellAt(0, 0)) && !blind.IsVisible(CellAt(3, 0)),
         "a blinded observer sees nothing");

  // Сильное затемнение: облако видно, луча за ним нет.
  dnd::GridMap smoke(8, 1);
  dnd::CellProps smoke_props = smoke.PropsAt(CellAt(2, 0));
  smoke_props.obscurement = dnd::Obscurement::kHeavy;
  smoke.SetProps(CellAt(2, 0), smoke_props);
  const dnd::LightField bright = dnd::ComputeLightField(smoke, {});
  dnd::FogOfWar smoke_fog(8, 1);
  smoke_fog.Recompute(smoke, {dnd::Observer{.cell = CellAt(0, 0)}}, bright, 0);
  Expect(smoke_fog.IsVisible(CellAt(2, 0)),
         "the smoke cloud itself is visible");
  Expect(!smoke_fog.IsVisible(CellAt(3, 0)), "smoke blocks sight");

  // Наблюдатели сливаются: тёмное зрение одного видит за всю партию.
  dnd::FogOfWar group(8, 1);
  group.Recompute(map,
                  {dnd::Observer{.cell = CellAt(0, 0)},
                   dnd::Observer{.cell = CellAt(7, 0), .darkvision_cells = 2}},
                  dark, 0);
  Expect(group.IsVisible(CellAt(6, 0)), "another observer's darkvision counts");
}

auto TestTokenLight() -> void {
  dnd::GridMap map(10, 1);
  map.SetAmbientLight(dnd::LightLevel::kDark);
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
                         .cell = CellAt(6, 0)},
          },
      .next_token_id = 3,
  };
  dnd::GameServer server(snapshot, 0);
  Expect(server.ClientSnapshot("alice").tokens.size() == 1,
         "a monster in the dark is hidden");

  const dnd::CommandOutcome foreign = server.HandleCommand(
      dnd::SetTokenLight{.id = 1, .bright_cells = 3, .dim_cells = 6}, "bob",
      false);
  Expect(!foreign.accepted, "a stranger cannot light another token");

  const dnd::CommandOutcome lit = server.HandleCommand(
      dnd::SetTokenLight{.id = 1, .bright_cells = 3, .dim_cells = 6}, "alice",
      false);
  Expect(lit.accepted, "the owner lights their own token");
  Expect(server.State().Tokens().front().light_bright_cells == 3 &&
             server.State().Tokens().front().light_dim_cells == 6,
         "token light is stored in cells");
  Expect(FogVisibleAt(server.FogState("alice"), 6, 0),
         "the torch lights the corridor for its owner");
  Expect(server.ClientSnapshot("alice").tokens.size() == 2,
         "a monster in the light is revealed");

  const dnd::CommandOutcome off =
      server.HandleCommand(dnd::SetTokenLight{.id = 1}, "alice", false);
  Expect(
      off.accepted && server.State().Tokens().front().light_bright_cells == 0,
      "an empty light puts the torch out");
  Expect(server.ClientSnapshot("alice").tokens.size() == 1,
         "the monster disappears in the dark again");

  // Дым прячет существо, хотя облако видно.
  dnd::GridMap smoky(10, 1);
  smoky.SetAmbientLight(dnd::LightLevel::kBright);
  dnd::CellProps smoke = smoky.PropsAt(CellAt(6, 0));
  smoke.obscurement = dnd::Obscurement::kHeavy;
  smoky.SetProps(CellAt(6, 0), smoke);
  dnd::GameSnapshot smoky_snapshot = snapshot;
  smoky_snapshot.map = smoky;
  dnd::GameServer smoke_server(smoky_snapshot, 0);
  Expect(FogVisibleAt(smoke_server.FogState("alice"), 6, 0),
         "the smoke cloud is visible to the party");
  Expect(smoke_server.ClientSnapshot("alice").tokens.size() == 1,
         "a token inside smoke is hidden");
}

// Ослеплённый персонаж не должен видеть партию: у игрока есть персонаж, значит
// его личный туман пуст, и партийный обзор ему не подменяет зрение.
auto TestBlindedCharacterKeepsOwnFog() -> void {
  dnd::GridMap map(10, 1);
  map.SetAmbientLight(dnd::LightLevel::kDark);
  dnd::GameSnapshot snapshot{
      .map = map,
      .tokens =
          {
              dnd::Token{.id = 1,
                         .name = "hero",
                         .owner = "alice",
                         .kind = dnd::TokenKind::kPlayer,
                         .cell = CellAt(0, 0)},
              // Второй персонаж партии без владельца: он даёт партийный туман,
              // которым не должен пользоваться ослеплённый игрок.
              dnd::Token{.id = 2,
                         .name = "ally",
                         .kind = dnd::TokenKind::kPlayer,
                         .cell = CellAt(9, 0)},
          },
      .next_token_id = 3,
  };
  dnd::GameServer server(snapshot, 0);
  Expect(FogVisibleAt(server.FogState("alice"), 0, 0),
         "a sighted character sees its own cell first");

  // Боевой путь: DM ставит состояние командой, а не правкой снимка.
  const dnd::CommandOutcome blind = server.HandleCommand(
      dnd::UpdateTokenCombat{.id = 1,
                             .conditions = {dnd::Condition("ослеплён")}},
      "", true);
  Expect(blind.accepted, "the DM blinds the hero by command");
  Expect(!FogVisibleAt(server.FogState("alice"), 0, 0),
         "blinding clears the personal fog right away");
  Expect(server.ClientSnapshot("alice").tokens.empty(),
         "blinding hides every token from that player");
  Expect(FogVisibleAt(server.FogState("bob"), 9, 0),
         "an unowned player still shares the party view");

  snapshot.tokens.front().conditions.emplace_back(std::string(dnd::kBlinded));
  dnd::GameServer blinded(snapshot, 0);
  Expect(!FogVisibleAt(blinded.FogState("alice"), 0, 0),
         "a blinded character sees nothing, not even its own cell");
  Expect(!FogVisibleAt(blinded.FogState("alice"), 9, 0),
         "a blinded player gets no party view");
  Expect(blinded.ClientSnapshot("alice").tokens.empty(),
         "a blinded player receives no tokens");
  Expect(FogVisibleAt(blinded.FogState("bob"), 9, 0),
         "a player without a character still shares the party view");
}

auto TestServerVisibility() -> void {
  dnd::GridMap map(10, 2);
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

  Expect(TokenOf(server.State(), 2).owner == "dm",
         "an unowned monster is given to the DM on load");

  const dnd::GameSnapshot client = server.ClientSnapshot("alice");
  Expect(client.tokens.size() == 1, "hidden token is not in client snapshot");
  Expect(client.tokens.front().id == 1, "visible token is in client snapshot");
  Expect(server.DmSnapshot().tokens.size() == 2, "DM snapshot sees everything");

  dnd::ClientView view;
  view.Reset(server.ClientSnapshot("alice"), server.FogState("alice"));
  Expect(view.Fog().IsVisible(CellAt(0, 0)), "view receives server fog");
  Expect(!view.Fog().IsVisible(CellAt(8, 0)), "goblin cell is hidden");

  const dnd::CommandOutcome hidden = server.HandleCommand(
      dnd::MoveToken{.id = 2, .target = CellAt(9, 0)}, "", true);
  Expect(hidden.accepted, "the DM may move a hidden token");
  bool hidden_move_visible = false;
  for (const dnd::Event& event : hidden.events) {
    if (std::holds_alternative<dnd::TokenMoved>(event)) {
      hidden_move_visible = server.EventVisibleTo(event, "alice");
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
      own_move_visible = server.EventVisibleTo(event, "alice");
    }
    view.Apply(event);
  }
  Expect(own_move_visible, "visible move is visible to players");
  Expect(view.Tokens().front().cell == CellAt(1, 0), "client view moves token");

  // Токен, скрытый при входе клиента, должен появиться, когда он попадёт
  // в поле зрения игрока. После команды приложение присылает полный срез.
  const dnd::CommandOutcome reveal = server.HandleCommand(
      dnd::MoveToken{.id = 2, .target = CellAt(4, 0)}, "", true);
  Expect(reveal.accepted, "goblin move into sight is accepted");
  for (const dnd::Event& event : reveal.events) {
    view.Apply(event);
  }
  view.Reset(server.ClientSnapshot("alice"), server.FogState("alice"));
  Expect(view.Tokens().size() == 2,
         "revealed token appears in the client view");
}

auto TestPendingMove() -> void {
  const dnd::GridMap map(8, 1);
  const auto snapshot = [&map](dnd::Cell cell) -> dnd::GameSnapshot {
    return dnd::GameSnapshot{
        .map = map,
        .tokens = {dnd::Token{.id = 1,
                              .name = "hero",
                              .owner = "alice",
                              .kind = dnd::TokenKind::kPlayer,
                              .cell = cell}},
        .next_token_id = 2,
    };
  };
  const dnd::FogSnapshot fog{.width = 8,
                             .height = 1,
                             .visible = std::vector<std::uint8_t>(8, 1),
                             .explored = std::vector<std::uint8_t>(8, 1)};
  dnd::ClientView view;
  view.Reset(snapshot(CellAt(0, 0)), fog);

  view.PredictMove(1, CellAt(3, 0), 100);
  Expect(view.PendingMoveCell(1) == CellAt(3, 0),
         "a pending move draws the token on the target cell");
  Expect(!view.PendingMoveCell(2).has_value(),
         "a pending move belongs only to its token");
  Expect(view.Tokens().front().cell == CellAt(0, 0),
         "the predicted move does not change the server copy");

  view.ExpirePendingMove(100 + dnd::kPendingMoveSeconds - 1);
  Expect(view.PendingMoveCell(1).has_value(),
         "the prediction survives until the timeout");
  view.ExpirePendingMove(100 + dnd::kPendingMoveSeconds);
  Expect(!view.PendingMoveCell(1).has_value(),
         "a lost answer drops the prediction after the timeout");

  view.PredictMove(1, CellAt(4, 0), 200);
  view.Apply(
      dnd::TokenMoved{.id = 1, .from = CellAt(0, 0), .to = CellAt(4, 0)});
  Expect(!view.PendingMoveCell(1).has_value(),
         "the server answer confirms and drops the prediction");
  Expect(view.Tokens().front().cell == CellAt(4, 0),
         "the confirmed move lands in the client view");

  view.PredictMove(1, CellAt(5, 0), 300);
  view.ClearPendingMove();
  Expect(!view.PendingMoveCell(1).has_value(),
         "a rejection clears the prediction");

  view.PredictMove(1, CellAt(6, 0), 400);
  view.Reset(snapshot(CellAt(6, 0)), fog);
  Expect(!view.PendingMoveCell(1).has_value(),
         "a full resync clears the prediction");
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

  Expect(server.ClientSnapshot("alice").tokens.size() == 1,
         "far goblin and ally are hidden from alice");
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

  // Монстр всегда принадлежит DM: освободить или передать его нельзя.
  Expect(!server.HandleCommand(dnd::ReleaseToken{.id = 2}, "", true).accepted,
         "the DM cannot release a monster");
  Expect(
      !server.HandleCommand(dnd::SetTokenOwner{.id = 2, .owner = ""}, "", true)
           .accepted,
      "a monster cannot lose its owner");
  Expect(!server
              .HandleCommand(dnd::SetTokenOwner{.id = 2, .owner = "alice"}, "",
                             true)
              .accepted,
         "a monster cannot be handed to a player");
  Expect(TokenOf(server.State(), 2).owner == "dm",
         "the monster stays with the DM");

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
      !server.HandleCommand(dnd::ClaimToken{.id = 1}, "alice", false).accepted,
      "a player cannot choose again after the DM released the character");
  Expect(server
             .HandleCommand(dnd::SetTokenOwner{.id = 1, .owner = "alice"}, "",
                            true)
             .accepted,
         "the DM assigns the released character");

  Expect(!server
              .HandleCommand(dnd::RenameToken{.id = 1, .name = "Алиса"},
                             "alice", false)
              .accepted,
         "players cannot rename the token");
  Expect(
      server.HandleCommand(dnd::RenameToken{.id = 1, .name = "Алиса"}, "", true)
          .accepted,
      "the DM renames the token");
  Expect(TokenOf(server.State(), 1).name == "Алиса",
         "the renamed value is stored");
  Expect(!server.HandleCommand(dnd::RenameToken{.id = 1, .name = ""}, "", true)
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
      dnd::SetTokenColor{.id = 1, .color = 0xC85046U}, "", true);
  Expect(color.accepted, "the DM can set the colour");
  Expect(TokenOf(server.State(), 1).color == 0xC85046U, "the colour is stored");
  Expect(!server
              .HandleCommand(dnd::SetTokenColor{.id = 1, .color = 0xC85046U},
                             "alice", false)
              .accepted,
         "players cannot set the colour");

  // Лист и состояния правит только DM: игрок их лишь смотрит.
  Expect(!server
              .HandleCommand(dnd::UpdateTokenSheet{.id = 1,
                                                   .ac = 16,
                                                   .speed = 9,
                                                   .notes = "Атака: +5, 1d8+3",
                                                   .attack_bonus = 5,
                                                   .save_bonus = 2,
                                                   .damage = "1d8+3",
                                                   .temp_hp = 4},
                             "alice", false)
              .accepted,
         "players cannot edit the character sheet");
  const dnd::CommandOutcome sheet =
      server.HandleCommand(dnd::UpdateTokenSheet{.id = 1,
                                                 .ac = 16,
                                                 .speed = 9,
                                                 .notes = "Атака: +5, 1d8+3",
                                                 .attack_bonus = 5,
                                                 .save_bonus = 2,
                                                 .damage = "1d8+3",
                                                 .temp_hp = 4},
                           "", true);
  Expect(sheet.accepted, "the DM can edit the character sheet");
  Expect(TokenOf(server.State(), 1).ac == 16, "КД сохраняется");
  Expect(TokenOf(server.State(), 1).speed == 9, "speed is stored");
  Expect(TokenOf(server.State(), 1).notes == "Атака: +5, 1d8+3",
         "notes are stored");
  Expect(TokenOf(server.State(), 1).attack_bonus == 5,
         "attack bonus is stored");
  Expect(TokenOf(server.State(), 1).save_bonus == 2, "save bonus is stored");
  Expect(TokenOf(server.State(), 1).damage == "1d8+3", "damage is stored");
  Expect(TokenOf(server.State(), 1).temp_hp == 4, "temporary HP are stored");
  Expect(std::ranges::any_of(sheet.events,
                             [](const dnd::Event& event) -> bool {
                               return std::holds_alternative<dnd::LogLineAdded>(
                                   event);
                             }),
         "sheet changes are written to the log");
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
               "", true)
           .accepted,
      "некорректный КД отвергается");
  Expect(!server
              .HandleCommand(
                  dnd::UpdateTokenSheet{
                      .id = 1, .ac = 12, .speed = 9, .temp_hp = 1001},
                  "", true)
              .accepted,
         "too much temporary HP is rejected");

  Expect(!server
              .HandleCommand(
                  dnd::UpdateTokenCombat{
                      .id = 1,
                      .conditions = {dnd::Condition{"сбит с ног"},
                                     dnd::Condition{"отравлен"}}},
                  "alice", false)
              .accepted,
         "players cannot update conditions");
  const dnd::CommandOutcome conditions = server.HandleCommand(
      dnd::UpdateTokenCombat{.id = 1,
                             .conditions = {dnd::Condition{"сбит с ног"},
                                            dnd::Condition{"отравлен"}}},
      "", true);
  Expect(conditions.accepted, "the DM can update conditions");
  Expect(TokenOf(server.State(), 1).conditions.size() == 2,
         "conditions are stored");
  Expect(std::ranges::any_of(conditions.events,
                             [](const dnd::Event& event) -> bool {
                               return std::holds_alternative<dnd::LogLineAdded>(
                                   event);
                             }),
         "condition changes are written to the log");
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
                  "", true)
              .accepted,
         "empty condition is rejected");

  Expect(!server.HandleCommand(dnd::ReleaseToken{.id = 1}, "alice", false)
              .accepted,
         "players cannot release the character");
  Expect(server.HandleCommand(dnd::ReleaseToken{.id = 1}, "", true).accepted,
         "the DM can release the character");
  Expect(TokenOf(server.State(), 1).owner.empty(),
         "released character becomes free");
  Expect(
      !server.HandleCommand(dnd::ClaimToken{.id = 3}, "alice", false).accepted,
      "the player cannot choose another character after the DM released");
  Expect(server
             .HandleCommand(dnd::SetTokenOwner{.id = 3, .owner = "alice"}, "",
                            true)
             .accepted,
         "the DM assigns another character instead");
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
                                        dnd::Condition{"отравлен"},
                                        dnd::Condition{"испуган",
                                                       dnd::kSecondsPerHour}},
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
  Expect(HasCondition(down, dnd::kUnconscious),
         "zero hp adds the unconscious condition");

  const dnd::CommandOutcome heal = server.HandleCommand(
      dnd::HealToken{.id = 1, .amount = 50}, "alice", false);
  Expect(heal.accepted, "the owner can heal their character");
  const dnd::Token healed = TokenOf(server.State(), 1);
  Expect(healed.hp == 20, "healing stops at max hp");
  Expect(!HasCondition(healed, dnd::kUnconscious),
         "healing removes the unconscious condition");

  Expect(
      !server.HandleCommand(dnd::RestParty{.long_rest = true}, "alice", false)
           .accepted,
      "only the DM declares a rest");

  // Отдых при 0 HP не даёт ничего: лежачий не встаёт и не тратит кости.
  Expect(server.HandleCommand(dnd::RestParty{}, "", true).accepted,
         "the DM declares a short rest");
  const dnd::Token down_rested = TokenOf(server.State(), 2);
  Expect(down_rested.hp == 0 && down_rested.hit_dice_spent == 0,
         "a character at zero hp gains nothing from a short rest");
  Expect(HasCondition(down_rested, dnd::kUnconscious) &&
             HasCondition(down_rested, "отравлен") &&
             !HasCondition(down_rested, "испуган"),
         "time still passes: the timed condition expires, the rest stay");

  const dnd::CommandOutcome wake =
      server.HandleCommand(dnd::HealToken{.id = 2, .amount = 5}, "bob", false);
  Expect(wake.accepted, "healing a downed character is allowed");
  const dnd::Token woken = TokenOf(server.State(), 2);
  Expect(woken.hp == 5 && !HasCondition(woken, dnd::kUnconscious),
         "healing wakes the character");

  Expect(server.HandleCommand(dnd::RestParty{}, "", true).accepted,
         "the DM declares a short rest");
  const dnd::Token rested = TokenOf(server.State(), 2);
  Expect(rested.hp > woken.hp && rested.hp <= rested.max_hp,
         "short rest spends a hit die instead of restoring half of max hp");
  Expect(rested.hit_dice_spent == 1,
         "short rest consumes one hit die at level 1");
  Expect(HasCondition(rested, "отравлен"),
         "short rest keeps conditions that are not timed");

  Expect(server.HandleCommand(dnd::RestParty{}, "", true).accepted,
         "a second short rest is allowed");
  const dnd::Token drained = TokenOf(server.State(), 2);
  Expect(drained.hp == rested.hp && drained.hit_dice_spent == 1,
         "a spent hit die cannot be reused before a long rest");

  // Два уровня истощения: длинный отдых снимает ровно один.
  Expect(server
             .HandleCommand(
                 dnd::UpdateTokenCombat{
                     .id = 2,
                     .conditions = {dnd::Condition{"отравлен"},
                                    dnd::Condition{"истощение"},
                                    dnd::Condition{"истощение"}}},
                 "", true)
             .accepted,
         "the DM sets conditions on the character");
  Expect(server.HandleCommand(dnd::RestParty{.long_rest = true}, "", true)
             .accepted,
         "the DM declares a long rest");
  const dnd::Token restored = TokenOf(server.State(), 2);
  Expect(restored.hp == restored.max_hp && restored.temp_hp == 0,
         "long rest restores hp");
  Expect(restored.hit_dice_spent == 0, "long rest returns every spent hit die");
  Expect(
      HasCondition(restored, "отравлен") && HasCondition(restored, "истощение"),
      "long rest keeps conditions other than one exhaustion level");
  const int exhaustion_levels = static_cast<int>(std::ranges::count_if(
      restored.conditions, [](const dnd::Condition& condition) -> bool {
        return condition.name == "истощение";
      }));
  Expect(exhaustion_levels == 1,
         "long rest removes exactly one exhaustion level");

  // Длинный отдых не поднимает лежачего и не воскрешает мёртвого.
  Expect(server.HandleCommand(dnd::DamageToken{.id = 2, .amount = 10}, "", true)
             .accepted,
         "the DM can drop the character to zero");
  // Следующий длинный отдых — не раньше чем через 16 часов.
  Expect(!server.HandleCommand(dnd::RestParty{.long_rest = true}, "", true)
              .accepted,
         "a long rest cannot repeat before 16 hours");
  Expect(
      server.HandleCommand(dnd::AdvanceTime{.seconds = 16 * 60 * 60}, "", true)
          .accepted,
      "the DM advances the clock between long rests");
  Expect(server.HandleCommand(dnd::RestParty{.long_rest = true}, "", true)
             .accepted,
         "a long rest over a downed party is allowed");
  const dnd::Token still_down = TokenOf(server.State(), 2);
  Expect(still_down.hp == 0 && HasCondition(still_down, dnd::kUnconscious),
         "long rest does not wake a character at zero hp");

  Expect(server.HandleCommand(dnd::DamageToken{.id = 2, .amount = 10}, "", true)
             .accepted,
         "massive damage on a downed character is applied");
  Expect(HasCondition(TokenOf(server.State(), 2), dnd::kDead),
         "damage equal to max hp kills a downed character");
  Expect(
      server.HandleCommand(dnd::AdvanceTime{.seconds = 16 * 60 * 60}, "", true)
          .accepted,
      "the clock advances before the last long rest");
  Expect(server.HandleCommand(dnd::RestParty{.long_rest = true}, "", true)
             .accepted,
         "the DM can still call a long rest");
  const dnd::Token dead = TokenOf(server.State(), 2);
  Expect(dead.hp == 0 && HasCondition(dead, dnd::kDead),
         "long rest does not revive the dead");
}

auto TestShortRestHitDice() -> void {
  const dnd::GridMap map(6, 6);
  const dnd::GameSnapshot snapshot{
      .map = map,
      .tokens =
          {
              dnd::Token{.id = 1,
                         .name = "hero",
                         .owner = "alice",
                         .hp = 3,
                         .max_hp = 200,
                         .hit_die = 12,
                         .con_modifier = 4,
                         .xp = 900,
                         .kind = dnd::TokenKind::kPlayer,
                         .cell = CellAt(1, 1)},
              // Слабый модификатор Телосложения: кость не может вылечить
              // меньше 1 HP, даже когда бросок ниже нуля.
              dnd::Token{.id = 2,
                         .name = "frail",
                         .owner = "bob",
                         .hp = 1,
                         .max_hp = 100,
                         .hit_die = 6,
                         .con_modifier = -5,
                         .xp = 900,
                         .kind = dnd::TokenKind::kPlayer,
                         .cell = CellAt(2, 2)},
          },
      .next_token_id = 3,
  };
  dnd::GameState state(snapshot);
  // Фиксированное зерно: тест проверяет границы лечения и учёт костей, а не
  // случайность как таковую.
  // NOLINTNEXTLINE(bugprone-random-generator-seed)
  std::mt19937 rng(2024);

  const std::vector<dnd::RestChange> changes = state.RestParty(false, rng);
  Expect(changes.size() == 2, "both wounded heroes are reported");
  const dnd::RestChange& hero = changes[0];
  Expect(hero.token.hit_dice_spent == 3,
         "a short rest spends a hit die per level while hp is missing");
  Expect(hero.dice_spent == 3 && hero.hp_restored == hero.token.hp - 3,
         "the rest report counts dice and restored hp");
  Expect(hero.token.hp >= 3 + (3 * 5),
         "each spent hit die heals d12 plus Constitution, at least one");
  const dnd::RestChange& frail = changes[1];
  Expect(frail.dice_spent == 3 && frail.hp_restored >= frail.dice_spent,
         "a hit die heals at least 1 HP even with a negative modifier");

  Expect(state.RestParty(false, rng).empty(),
         "a short rest without dice left changes nothing");

  const std::vector<dnd::RestChange> long_rest = state.RestParty(true, rng);
  Expect(long_rest.size() == 2, "a long rest reports both heroes");
  Expect(long_rest[0].token.hp == 200 && long_rest[0].token.hit_dice_spent == 0,
         "a long rest restores hp and every hit die");
  Expect(long_rest[1].token.hp == 100 && long_rest[1].token.hit_dice_spent == 0,
         "a long rest restores the frail hero too");
}

auto TestRestChoicesAndClock() -> void {
  const dnd::GridMap map(8, 8);
  const dnd::GameSnapshot snapshot{
      .map = map,
      .tokens =
          {
              dnd::Token{.id = 1,
                         .name = "hero",
                         .owner = "alice",
                         .hp = 1,
                         .max_hp = 100,
                         .hit_die = 8,
                         .con_modifier = 2,
                         .xp = 6500,
                         .kind = dnd::TokenKind::kPlayer,
                         .cell = CellAt(1, 1)},
              dnd::Token{.id = 2,
                         .name = "downed",
                         .owner = "bob",
                         .hp = 1,
                         .max_hp = 10,
                         .kind = dnd::TokenKind::kPlayer,
                         .cell = CellAt(2, 2)},
          },
      .next_token_id = 3,
  };
  dnd::GameServer server(snapshot, 6, 2024);

  // В бою отдых прерывает инициатива.
  Expect(server.HandleCommand(dnd::StartEncounter{}, "", true).accepted,
         "the encounter starts");
  const dnd::CommandOutcome in_combat =
      server.HandleCommand(dnd::RestParty{}, "", true);
  Expect(!in_combat.accepted &&
             in_combat.reject_reason.find("инициатива") != std::string::npos,
         "combat rejects a rest");
  Expect(server.HandleCommand(dnd::EndEncounter{}, "", true).accepted,
         "the encounter ends");

  // Игрок сам выбирает, сколько костей потратить.
  const dnd::CommandOutcome one_die =
      server.HandleCommand(dnd::RestParty{.hit_dice = 1}, "", true);
  Expect(one_die.accepted, "a short rest with one hit die is accepted");
  Expect(TokenOf(server.State(), 1).hit_dice_spent == 1,
         "exactly the chosen number of hit dice is spent");
  const int hp_after_one = TokenOf(server.State(), 1).hp;
  Expect(hp_after_one > 1 && hp_after_one < 100,
         "one hit die heals but does not fill the character");

  const dnd::CommandOutcome auto_dice =
      server.HandleCommand(dnd::RestParty{}, "", true);
  Expect(auto_dice.accepted, "a short rest without a limit is accepted");
  Expect(TokenOf(server.State(), 1).hit_dice_spent > 1,
         "the automatic rest spends more dice until full");

  // Длинный отдых — не чаще чем раз в 16 часов.
  const int before_long = server.State().GameTimeSeconds();
  Expect(server.HandleCommand(dnd::RestParty{.long_rest = true}, "", true)
             .accepted,
         "the first long rest is allowed");
  Expect(server.State().LongRestReadyInSeconds() ==
             dnd::kLongRestCooldownSeconds - 8 * dnd::kSecondsPerHour,
         "a long rest leaves eight more hours of cooldown");
  const dnd::CommandOutcome too_soon =
      server.HandleCommand(dnd::RestParty{.long_rest = true}, "", true);
  Expect(!too_soon.accepted &&
             too_soon.reject_reason.find("не раньше") != std::string::npos,
         "the second long rest is rejected with a clear reason");
  Expect(server.State().GameTimeSeconds() ==
             before_long + 8 * dnd::kSecondsPerHour,
         "a long rest advances the clock by eight hours");
  Expect(
      server.HandleCommand(dnd::AdvanceTime{.seconds = 8 * 60 * 60}, "", true)
          .accepted,
      "the clock advances after the long rest");
  Expect(server.State().LongRestReadyInSeconds() == 0,
         "sixteen hours after the start the next long rest is ready");
  Expect(server.HandleCommand(dnd::RestParty{.long_rest = true}, "", true)
             .accepted,
         "the next long rest is allowed after the cooldown");

  // Лежачему отдых не помогает: при нуле HP у всех персонажей команда
  // отклоняется с понятной причиной.
  Expect(
      server.HandleCommand(dnd::DamageToken{.id = 1, .amount = 200}, "", true)
          .accepted,
      "the DM drops the hero");
  Expect(
      server.HandleCommand(dnd::DamageToken{.id = 2, .amount = 200}, "", true)
          .accepted,
      "the DM drops the second hero");
  const dnd::CommandOutcome downed =
      server.HandleCommand(dnd::RestParty{}, "", true);
  Expect(!downed.accepted &&
             downed.reject_reason.find("0 HP") != std::string::npos,
         "a party lying at zero hp cannot rest");
}

auto TestConcentration() -> void {
  Expect(dnd::ConcentrationDc(5) == 10 && dnd::ConcentrationDc(20) == 10 &&
             dnd::ConcentrationDc(21) == 10 && dnd::ConcentrationDc(100) == 30,
         "concentration DC is 10 or half damage, capped at 30");

  const dnd::GridMap map(8, 8);
  const auto concentrator = [](dnd::TokenId id, const char* name, int hp,
                               int save_bonus, int x, int y) -> dnd::Token {
    return dnd::Token{.id = id,
                      .name = name,
                      .owner = name,
                      .hp = hp,
                      .max_hp = 10,
                      .save_bonus = save_bonus,
                      .conditions = {dnd::Condition{"концентрация"}},
                      .kind = dnd::TokenKind::kPlayer,
                      .cell = CellAt(x, y)};
  };
  const dnd::GameSnapshot snapshot{
      .map = map,
      .tokens =
          {
              concentrator(1, "calm", 10, 100, 1, 1),
              concentrator(2, "shaken", 10, -100, 2, 2),
              concentrator(3, "dying", 1, 100, 3, 3),
              concentrator(4, "slain", 1, 100, 4, 4),
              concentrator(5, "stunned", 10, 100, 5, 5),
          },
      .next_token_id = 6,
  };
  dnd::GameServer server(snapshot, 6, 9);

  Expect(
      server
          .HandleCommand(dnd::DamageToken{.id = 1, .amount = 7}, "calm", false)
          .accepted,
      "damage with concentration is accepted");
  Expect(TokenOf(server.State(), 1).conditions.size() == 1,
         "a successful save keeps concentration");

  Expect(server
             .HandleCommand(dnd::DamageToken{.id = 2, .amount = 7}, "shaken",
                            false)
             .accepted,
         "damage with concentration is accepted");
  Expect(TokenOf(server.State(), 2).conditions.empty(),
         "a failed concentration save removes the condition");

  // 0 HP, смерть и недееспособность рвут концентрацию без спасброска.
  Expect(
      server
          .HandleCommand(dnd::DamageToken{.id = 3, .amount = 5}, "dying", false)
          .accepted,
      "damage dropping the character to zero is accepted");
  Expect(!HasCondition(TokenOf(server.State(), 3), dnd::kConcentration),
         "zero hp breaks concentration");

  Expect(server
             .HandleCommand(dnd::DamageToken{.id = 4, .amount = 20}, "slain",
                            false)
             .accepted,
         "massive damage is accepted");
  const dnd::Token slain = TokenOf(server.State(), 4);
  Expect(HasCondition(slain, dnd::kDead) &&
             !HasCondition(slain, dnd::kConcentration),
         "death breaks concentration");

  Expect(server
             .HandleCommand(
                 dnd::UpdateTokenCombat{
                     .id = 5,
                     .conditions = {dnd::Condition{"концентрация"},
                                    dnd::Condition{"оглушён"}}},
                 "", true)
             .accepted,
         "the DM can apply an incapacitating condition");
  const dnd::Token stunned = TokenOf(server.State(), 5);
  Expect(HasCondition(stunned, "оглушён") &&
             !HasCondition(stunned, dnd::kConcentration),
         "incapacitation breaks concentration");
}

auto TestAttack() -> void {
  const dnd::AttackCheck critical = dnd::CheckAttack(20, -100, 1000);
  Expect(critical.hit && critical.critical,
         "natural 20 hits and crits even against a huge AC");
  const dnd::AttackCheck fumble = dnd::CheckAttack(1, 100, 1);
  Expect(!fumble.hit && !fumble.critical, "natural 1 always misses");
  Expect(dnd::CheckAttack(10, 5, 15).hit, "a total equal to AC hits");
  Expect(!dnd::CheckAttack(9, 5, 15).hit, "a total below AC misses");

  const dnd::DiceExpression damage{.count = 2, .sides = 6, .modifier = 3};
  std::mt19937 rng(std::random_device{}());
  for (int i = 0; i < 50; ++i) {
    const dnd::DiceRollResult normal =
        dnd::RollAttackDamage(damage, false, rng);
    Expect(normal.total >= 5 && normal.total <= 15,
           "normal damage adds the modifier once");
    const dnd::DiceRollResult crit = dnd::RollAttackDamage(damage, true, rng);
    Expect(crit.total >= 7 && crit.total <= 27 && crit.rolls.size() == 4,
           "a crit doubles the dice and adds the modifier once");
  }

  const dnd::GridMap map(8, 8);
  const dnd::GameSnapshot snapshot{
      .map = map,
      .tokens =
          {
              dnd::Token{.id = 1,
                         .name = "hero",
                         .owner = "alice",
                         .hp = 30,
                         .max_hp = 30,
                         .ac = 10,
                         .attack_bonus = 100,
                         .damage = "1d4",
                         .kind = dnd::TokenKind::kPlayer,
                         .cell = CellAt(1, 1)},
              dnd::Token{.id = 2,
                         .name = "goblin",
                         .owner = "dm",
                         .hp = 100000,
                         .max_hp = 100000,
                         .ac = 10,
                         .kind = dnd::TokenKind::kMonster,
                         .cell = CellAt(1, 2)},
              dnd::Token{.id = 3,
                         .name = "hidden",
                         .owner = "dm",
                         .hp = 10,
                         .max_hp = 10,
                         .ac = 10,
                         .kind = dnd::TokenKind::kMonster,
                         .cell = CellAt(6, 6)},
          },
      .next_token_id = 4,
  };
  dnd::GameServer server(snapshot, 1, 23);

  Expect(!server
              .HandleCommand(dnd::AttackToken{.attacker = 1, .target = 1},
                             "alice", false)
              .accepted,
         "a token cannot attack itself");
  Expect(!server
              .HandleCommand(dnd::AttackToken{.attacker = 1, .target = 2},
                             "bob", false)
              .accepted,
         "a stranger does not control the attacker");
  Expect(!server
              .HandleCommand(dnd::AttackToken{.attacker = 1, .target = 3},
                             "alice", false)
              .accepted,
         "a target outside the player's sight is rejected");

  dnd::GridMap water_map(4, 1);
  water_map.SetTerrain(CellAt(1, 0), dnd::Terrain::kLava);
  water_map.SetTerrain(CellAt(2, 0), dnd::Terrain::kWater);
  const dnd::GameSnapshot water_snapshot{
      .map = water_map,
      .tokens = {dnd::Token{.id = 1,
                            .name = "archer",
                            .owner = "alice",
                            .hp = 10,
                            .max_hp = 10,
                            .ac = 12,
                            .kind = dnd::TokenKind::kPlayer,
                            .cell = CellAt(0, 0)},
                 dnd::Token{.id = 2,
                            .name = "goblin",
                            .owner = "dm",
                            .hp = 10,
                            .max_hp = 10,
                            .ac = 10,
                            .kind = dnd::TokenKind::kMonster,
                            .cell = CellAt(3, 0)}},
      .next_token_id = 3,
  };
  dnd::GameServer water_server(water_snapshot, 6, 29);
  Expect(water_server
             .HandleCommand(dnd::AttackToken{.attacker = 1, .target = 2},
                            "alice", false)
             .accepted,
         "lava and water block movement but not attacks across them");
  Expect(!server
              .HandleCommand(dnd::AttackToken{.attacker = 1,
                                              .target = 2,
                                              .advantage = true,
                                              .disadvantage = true},
                             "alice", false)
              .accepted,
         "advantage and disadvantage together are rejected");

  // Проверяем не конкретный бросок, а правило: HP падает ровно тогда, когда
  // в логе записано попадание. 200 атак делают хотя бы одно попадание
  // практически неизбежным (бонус 100 против КД 10).
  const auto run_attack = [&server](bool advantage, bool disadvantage,
                                    std::string_view mode_word) -> bool {
    const int hp_before = TokenOf(server.State(), 2).hp;
    const dnd::CommandOutcome outcome =
        server.HandleCommand(dnd::AttackToken{.attacker = 1,
                                              .target = 2,
                                              .advantage = advantage,
                                              .disadvantage = disadvantage},
                             "alice", false);
    if (!outcome.accepted) {
      return false;
    }
    bool hit = false;
    bool mode = mode_word.empty();
    bool ac = false;
    for (const dnd::Event& event : outcome.events) {
      const auto* line = std::get_if<dnd::LogLineAdded>(&event);
      if (line == nullptr) {
        continue;
      }
      const std::string& text = line->line.text;
      hit = hit || text.find("попадание") != std::string::npos;
      mode = mode || text.find(mode_word) != std::string::npos;
      ac = ac || text.find("против КД 10") != std::string::npos;
    }
    const bool damaged = TokenOf(server.State(), 2).hp < hp_before;
    return mode && ac && hit == damaged;
  };

  Expect(run_attack(false, false, ""),
         "a plain attack applies damage on a hit");
  Expect(run_attack(true, false, "с преимуществом"),
         "an advantaged attack is logged and resolved");
  Expect(run_attack(false, true, "с помехой"),
         "a disadvantaged attack is logged and resolved");
  bool saw_hit = false;
  for (int i = 0; i < 200; ++i) {
    const int hp_before = TokenOf(server.State(), 2).hp;
    Expect(run_attack(false, false, ""),
           "every attack applies damage only on a hit");
    saw_hit = saw_hit || TokenOf(server.State(), 2).hp < hp_before;
  }
  Expect(saw_hit, "a long series of attacks lands at least once");
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
  const auto downed = [](dnd::TokenId id, const char* name, int x,
                         int y) -> dnd::Token {
    return dnd::Token{.id = id,
                      .name = name,
                      .owner = "party",
                      .hp = 0,
                      .max_hp = 10,
                      .conditions = {dnd::Condition{"без сознания"}},
                      .kind = dnd::TokenKind::kPlayer,
                      .cell = CellAt(x, y)};
  };
  const dnd::GameSnapshot snapshot{
      .map = map,
      .tokens =
          {
              downed(1, "doomed", 1, 1),
              downed(2, "stable", 2, 2),
              downed(3, "lucky", 3, 3),
              downed(4, "hurt", 4, 4),
              downed(5, "smashed", 5, 5),
              downed(6, "critical", 6, 6),
              dnd::Token{.id = 7,
                         .name = "standing",
                         .owner = "party",
                         .hp = 10,
                         .max_hp = 10,
                         .kind = dnd::TokenKind::kPlayer,
                         .cell = CellAt(7, 1)},
              dnd::Token{.id = 8,
                         .name = "bruised",
                         .owner = "party",
                         .hp = 5,
                         .max_hp = 20,
                         .kind = dnd::TokenKind::kPlayer,
                         .cell = CellAt(7, 2)},
          },
      .next_token_id = 9,
  };
  dnd::GameState state(snapshot);

  // Натуральная 1 — два провала, а не один.
  (void)state.DeathSave(1, 5);
  const std::optional<dnd::Token> doomed = state.DeathSave(1, 1);
  Expect(doomed.has_value() && doomed->death_save_failures == 3,
         "a natural 1 adds two death save failures");
  if (doomed.has_value()) {
    Expect(HasCondition(*doomed, dnd::kDead) &&
               !HasCondition(*doomed, dnd::kUnconscious),
           "three failures kill the character");
  }
  Expect(!state.DeathSave(1, 10).has_value(),
         "a dead character makes no death saves");

  // Стабильность оставляет «без сознания» до первого лечения.
  (void)state.DeathSave(2, 10);
  (void)state.DeathSave(2, 11);
  const std::optional<dnd::Token> stable = state.DeathSave(2, 12);
  Expect(stable.has_value() && stable->death_save_successes == 3,
         "three successes stabilize the character");
  if (stable.has_value()) {
    Expect(HasCondition(*stable, dnd::kUnconscious),
           "a stable character stays unconscious at zero hp");
  }
  Expect(!state.DeathSave(2, 1).has_value(),
         "a stable character makes no more death saves");
  const std::optional<dnd::Token> healed = state.HealToken(2, 5);
  Expect(healed.has_value() && healed->hp == 5 &&
             !HasCondition(*healed, dnd::kUnconscious),
         "healing wakes a stable character");

  const std::optional<dnd::Token> revived = state.DeathSave(3, 20);
  Expect(revived.has_value() && revived->hp == 1 &&
             !HasCondition(*revived, dnd::kUnconscious),
         "a natural twenty brings the character back with 1 HP");

  // Удар по лежачему: один провал, критический — два.
  const std::optional<dnd::Token> hit = state.DamageToken(4, 4);
  Expect(hit.has_value() && hit->death_save_failures == 1,
         "damage at zero hp adds one death save failure");
  const std::optional<dnd::Token> critical = state.DamageToken(6, 4, true);
  Expect(critical.has_value() && critical->death_save_failures == 2,
         "a critical hit at zero hp adds two death save failures");

  const std::optional<dnd::Token> smashed = state.DamageToken(5, 10);
  Expect(smashed.has_value() && HasCondition(*smashed, dnd::kDead),
         "damage equal to max hp kills a downed character outright");

  const std::optional<dnd::Token> burst = state.DamageToken(7, 20);
  Expect(burst.has_value() && HasCondition(*burst, dnd::kDead),
         "massive damage kills even a standing character");
  const std::optional<dnd::Token> survived = state.DamageToken(8, 20);
  Expect(survived.has_value() && survived->hp == 0 &&
             !HasCondition(*survived, dnd::kDead),
         "damage below max hp only drops a standing character");
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
      dnd::UpdateTokenCombat{
          .id = 1,
          .conditions = {dnd::Condition{"отравлен", dnd::kSecondsPerRound},
                         dnd::Condition{"без сознания"}}},
      "", true);
  Expect(timed.accepted, "the DM sets a timed condition");
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

  // Вне боя время двигает только DM, и только когда бой не идёт.
  Expect(!server.HandleCommand(dnd::AdvanceTime{.seconds = 60}, "alice", false)
              .accepted,
         "players cannot advance time");
  Expect(
      !server.HandleCommand(dnd::AdvanceTime{.seconds = 60}, "", true).accepted,
      "time does not advance by hand during combat");
  Expect(server.HandleCommand(dnd::EndEncounter{}, "", true).accepted,
         "the DM ends the encounter");
  Expect(server
             .HandleCommand(
                 dnd::UpdateTokenCombat{
                     .id = 1,
                     .conditions = {dnd::Condition{"отравлен", 120},
                                    dnd::Condition{"без сознания"}}},
                 "", true)
             .accepted,
         "the DM sets long and permanent conditions");
  const dnd::CommandOutcome step =
      server.HandleCommand(dnd::AdvanceTime{.seconds = 60}, "", true);
  Expect(step.accepted, "the DM advances time out of combat");
  const dnd::Token walked = TokenOf(server.State(), 1);
  Expect(
      walked.conditions.size() == 2 && walked.conditions.front().seconds == 60,
      "time advances timed conditions outside combat");
  Expect(std::ranges::any_of(step.events,
                             [](const dnd::Event& event) -> bool {
                               return std::holds_alternative<dnd::LogLineAdded>(
                                   event);
                             }),
         "advancing time is written to the log");
  Expect(
      !server.HandleCommand(dnd::AdvanceTime{.seconds = 0}, "", true).accepted,
      "zero time is rejected");
  Expect(
      server.HandleCommand(dnd::AdvanceTime{.seconds = 60}, "", true).accepted,
      "the DM advances time again");
  const dnd::Token expired = TokenOf(server.State(), 1);
  Expect(expired.conditions.size() == 1 &&
             expired.conditions.front().name == "без сознания",
         "expired conditions are removed after enough time");

  Expect(dnd::FormatConditionDuration(0) == "до снятия",
         "zero duration means until removed");
  Expect(dnd::FormatConditionDuration(6) == "1 ход", "one round is one turn");
  Expect(dnd::FormatConditionDuration(60) == "1 минута",
         "a minute is formatted as a minute");
  Expect(dnd::FormatConditionDuration(600) == "10 минут",
         "long durations use minutes");
  Expect(dnd::FormatConditionDuration(7200) == "2 часа",
         "hours are used for long durations");
}

auto TestConditionSaves() -> void {
  const dnd::GridMap map(8, 8);
  const dnd::GameSnapshot snapshot{
      .map = map,
      .tokens =
          {
              dnd::Token{.id = 1,
                         .name = "hero",
                         .initiative = 20,
                         .save_bonus = 100,
                         .initiative_bonus = 100,
                         .kind = dnd::TokenKind::kPlayer,
                         .cell = CellAt(1, 1)},
              dnd::Token{.id = 2,
                         .name = "weak",
                         .initiative = 10,
                         .save_bonus = -100,
                         .initiative_bonus = -100,
                         .kind = dnd::TokenKind::kPlayer,
                         .cell = CellAt(2, 2)},
          },
      .next_token_id = 3,
  };
  dnd::GameServer server(snapshot, 6, 1234);
  Expect(server.HandleCommand(dnd::StartEncounter{}, "", true).accepted,
         "the DM starts the encounter for condition saves");

  Expect(server
             .HandleCommand(
                 dnd::UpdateTokenCombat{
                     .id = 1,
                     .conditions = {dnd::Condition{"отравлен", 0, 15},
                                    dnd::Condition{"испуган"}}},
                 "", true)
             .accepted,
         "the DM sets a condition with a save DC");
  const dnd::CommandOutcome saved =
      server.HandleCommand(dnd::NextTurn{}, "", true);
  Expect(saved.accepted, "the turn ends and saves are rolled");
  const dnd::Token after_success = TokenOf(server.State(), 1);
  Expect(after_success.conditions.size() == 1 &&
             after_success.conditions.front().name == "испуган",
         "a successful save removes the condition, permanent stays");
  Expect(std::ranges::any_of(
             saved.events,
             [](const dnd::Event& event) -> bool {
               if (!std::holds_alternative<dnd::LogLineAdded>(event)) {
                 return false;
               }
               const dnd::LogLine& line =
                   std::get<dnd::LogLineAdded>(event).line;
               return line.text.find("против КС 15") != std::string::npos;
             }),
         "the save roll is written to the log with КС");

  Expect(
      server
          .HandleCommand(
              dnd::UpdateTokenCombat{
                  .id = 2, .conditions = {dnd::Condition{"захвачен", 0, 15}}},
              "", true)
          .accepted,
      "the DM sets a condition on the failed saver");
  Expect(server.HandleCommand(dnd::NextTurn{}, "", true).accepted,
         "the turn ends again");
  const dnd::Token after_failure = TokenOf(server.State(), 2);
  Expect(after_failure.conditions.size() == 1 &&
             after_failure.conditions.front().name == "захвачен",
         "a failed save keeps the condition");

  const dnd::CommandOutcome dc_changed = server.HandleCommand(
      dnd::UpdateTokenCombat{.id = 2,
                             .conditions = {dnd::Condition{"захвачен", 0, 18}}},
      "", true);
  Expect(dc_changed.accepted &&
             std::ranges::any_of(
                 dc_changed.events,
                 [](const dnd::Event& event) -> bool {
                   if (!std::holds_alternative<dnd::LogLineAdded>(event)) {
                     return false;
                   }
                   return std::get<dnd::LogLineAdded>(event).line.text.find(
                              "КС 15→18") != std::string::npos;
                 }),
         "changing the save DC is written to the log");

  Expect(
      !server
           .HandleCommand(
               dnd::UpdateTokenCombat{
                   .id = 1, .conditions = {dnd::Condition{"отравлен", 0, 31}}},
               "", true)
           .accepted,
      "a save DC above 30 is rejected");
}

auto TestConditionPaletteAndExhaustion() -> void {
  const auto& palette = dnd::DefaultConditions();
  bool all_listed = true;
  for (const char* name :
       {"ослеплён", "очарован", "оглох", "испуган", "захвачен",
        "выведен из строя", "невидим", "парализован", "окаменел", "отравлен",
        "сбит с ног", "опутан", "оглушён", "без сознания", "истощение",
        "застигнут врасплох"}) {
    all_listed =
        all_listed && std::ranges::find(palette, name) != palette.end();
  }
  Expect(all_listed, "palette lists all 2024 conditions");

  dnd::Token token;
  token.speed = 9;
  token.conditions = {dnd::Condition{"истощение"}, dnd::Condition{"истощение"}};
  Expect(dnd::ExhaustionLevel(token) == 2, "two entries are level two");
  Expect(dnd::ExhaustionPenalty(token) == 4, "each level gives -2 to d20");
  Expect(dnd::ExhaustionSpeedPenaltyCells(token) == 2,
         "each level costs 5 feet of speed");
  Expect(dnd::EffectiveSpeedCells(token) == 4,
         "speed in cells drops by the exhaustion level");
  token.conditions.emplace_back("захвачен");
  Expect(dnd::IsSpeedZeroed(token), "grappled speed is zero");
  Expect(dnd::EffectiveSpeedCells(token) == 0,
         "grappled speed has no movement budget");
  token.conditions.clear();
  token.conditions.resize(dnd::kMaxExhaustionLevel,
                          dnd::Condition{"истощение"});
  Expect(dnd::ExhaustionLevel(token) == dnd::kMaxExhaustionLevel,
         "exhaustion is capped at six levels");

  const dnd::GridMap map(8, 8);
  const dnd::GameSnapshot snapshot{
      .map = map,
      .tokens =
          {
              dnd::Token{.id = 1,
                         .name = "hero",
                         .owner = "alice",
                         .hp = 10,
                         .max_hp = 10,
                         .kind = dnd::TokenKind::kPlayer,
                         .cell = CellAt(1, 1)},
              dnd::Token{.id = 2,
                         .name = "double",
                         .owner = "dm",
                         .hp = 10,
                         .max_hp = 10,
                         .kind = dnd::TokenKind::kMonster,
                         .cell = CellAt(4, 4)},
          },
      .next_token_id = 3,
  };
  dnd::GameServer server(snapshot, 6, 99);
  const std::vector<dnd::Condition> maximum(dnd::kMaxExhaustionLevel,
                                            dnd::Condition{"истощение"});
  const dnd::CommandOutcome killed = server.HandleCommand(
      dnd::UpdateTokenCombat{.id = 2, .conditions = maximum}, "", true);
  Expect(killed.accepted, "the DM sets six exhaustion levels");
  const dnd::Token dead = TokenOf(server.State(), 2);
  Expect(dead.hp == 0 && HasCondition(dead, dnd::kDead),
         "six exhaustion levels kill the character");
  Expect(std::ranges::any_of(
             killed.events,
             [](const dnd::Event& event) -> bool {
               if (!std::holds_alternative<dnd::LogLineAdded>(event)) {
                 return false;
               }
               return std::get<dnd::LogLineAdded>(event).line.text.find(
                          "истощение 6") != std::string::npos;
             }),
         "exhaustion death is written to the log");
  std::vector<dnd::Condition> too_many = maximum;
  too_many.emplace_back("истощение");
  Expect(!server
              .HandleCommand(
                  dnd::UpdateTokenCombat{.id = 1, .conditions = too_many}, "",
                  true)
              .accepted,
         "seven exhaustion levels are rejected");

  // Длинный отдых снимает ровно один уровень. Каждый шаг уровня истощения
  // пишется в лог, хотя записи состояния различаются только количеством.
  const auto log_has = [](const dnd::CommandOutcome& outcome,
                          std::string_view text) -> bool {
    return std::ranges::any_of(
        outcome.events, [text](const dnd::Event& event) -> bool {
          if (!std::holds_alternative<dnd::LogLineAdded>(event)) {
            return false;
          }
          return std::get<dnd::LogLineAdded>(event).line.text.find(text) !=
                 std::string::npos;
        });
  };
  Expect(server
             .HandleCommand(
                 dnd::UpdateTokenCombat{
                     .id = 1, .conditions = {dnd::Condition{"истощение"}}},
                 "", true)
             .accepted,
         "the DM applies one exhaustion level");
  const dnd::CommandOutcome raised = server.HandleCommand(
      dnd::UpdateTokenCombat{.id = 1,
                             .conditions = {dnd::Condition{"истощение"},
                                            dnd::Condition{"истощение"}}},
      "", true);
  Expect(raised.accepted, "the DM raises exhaustion to two levels");
  Expect(log_has(raised, "истощение 2/6"),
         "raising exhaustion is written to the log");
  const dnd::CommandOutcome lowered = server.HandleCommand(
      dnd::UpdateTokenCombat{.id = 1,
                             .conditions = {dnd::Condition{"истощение"}}},
      "", true);
  Expect(lowered.accepted, "the DM lowers exhaustion to one level");
  Expect(log_has(lowered, "истощение 1/6"),
         "lowering exhaustion is written to the log");
  Expect(server
             .HandleCommand(
                 dnd::UpdateTokenCombat{
                     .id = 1,
                     .conditions = {dnd::Condition{"истощение"},
                                    dnd::Condition{"истощение"}}},
                 "", true)
             .accepted,
         "the DM applies two exhaustion levels");
  Expect(server.HandleCommand(dnd::RestParty{.long_rest = true}, "", true)
             .accepted,
         "the party rests");
  Expect(dnd::ExhaustionLevel(TokenOf(server.State(), 1)) == 1,
         "a long rest removes one exhaustion level");

  // Истощение вычитается и из спасброска от смерти: 10 при −2 — провал.
  const dnd::GameSnapshot dying{
      .map = map,
      .tokens = {dnd::Token{.id = 1,
                            .name = "dying",
                            .hp = 0,
                            .max_hp = 10,
                            .conditions = {dnd::Condition{"без сознания"},
                                           dnd::Condition{"истощение"}},
                            .kind = dnd::TokenKind::kPlayer,
                            .cell = CellAt(0, 0)}},
      .next_token_id = 2,
  };
  dnd::GameState dying_state(dying);
  const std::optional<dnd::Token> failed = dying_state.DeathSave(1, 10, 2);
  Expect(failed.has_value() && failed->death_save_failures == 1,
         "exhaustion lowers a death save result");
  const std::optional<dnd::Token> natural = dying_state.DeathSave(1, 20, 2);
  Expect(natural.has_value() && natural->hp == 1,
         "a natural 20 on a death save still revives");
}

auto TestConditionAttackModifiers() -> void {
  dnd::Token attacker;
  dnd::Token target;
  Expect(dnd::AttackRollModesFor(attacker, target, 1, 0, false).Net() == 0,
         "plain conditions give a plain attack");

  target.conditions = {dnd::Condition{"сбит с ног"}};
  Expect(dnd::AttackRollModesFor(attacker, target, 1, 0, false).Net() == 1,
         "a prone target is easier to hit within five feet");
  Expect(dnd::AttackRollModesFor(attacker, target, 3, 0, false).Net() == -1,
         "a prone target is harder to hit at range");

  target.conditions = {dnd::Condition{"невидим"}};
  Expect(dnd::AttackRollModesFor(attacker, target, 1, 0, false).Net() == -1,
         "an invisible target is harder to hit");
  attacker.conditions = {dnd::Condition{"ослеплён"}};
  Expect(dnd::AttackRollModesFor(attacker, target, 1, 0, false).Net() == -1,
         "a blinded attacker has disadvantage");

  attacker.conditions = {dnd::Condition{"невидим"}};
  Expect(
      dnd::AttackRollModesFor(attacker, target, 1, 0, false).advantage &&
          dnd::AttackRollModesFor(attacker, target, 1, 0, false).disadvantage,
      "an invisible attacker against an invisible target has both");

  dnd::Token prone_target;
  prone_target.conditions = {dnd::Condition{"парализован"}};
  Expect(dnd::IsAutoCriticalOnHit(prone_target, 1),
         "a hit on a paralyzed target within five feet is critical");
  Expect(!dnd::IsAutoCriticalOnHit(prone_target, 2),
         "a paralyzed target at range is not automatically critical");

  dnd::Token archer;
  archer.attack_range = 2;
  const dnd::Token nearby;
  Expect(dnd::AttackRollModesFor(archer, nearby, 2, 2, false).Net() == 0,
         "a ranged attack inside normal range is plain");
  Expect(dnd::AttackRollModesFor(archer, nearby, 3, 2, false).Net() == -1,
         "a ranged attack beyond normal range has disadvantage");
  Expect(dnd::AttackRollModesFor(archer, nearby, 2, 2, true).Net() == -1,
         "a ranged attack next to another enemy has disadvantage");

  const dnd::GridMap map(8, 1);
  const dnd::GameSnapshot snapshot{
      .map = map,
      .tokens =
          {
              dnd::Token{.id = 1,
                         .name = "hero",
                         .owner = "alice",
                         .hp = 30,
                         .max_hp = 30,
                         .ac = 10,
                         .attack_bonus = 100,
                         .damage = "1d4",
                         .conditions = {dnd::Condition{"ослеплён"}},
                         .kind = dnd::TokenKind::kPlayer,
                         .cell = CellAt(0, 0)},
              dnd::Token{.id = 2,
                         .name = "goblin",
                         .owner = "dm",
                         .hp = 1000,
                         .max_hp = 1000,
                         .ac = 10,
                         .kind = dnd::TokenKind::kMonster,
                         .cell = CellAt(1, 0)},
          },
      .next_token_id = 3,
  };
  dnd::GameServer server(snapshot, 6, 5);
  const dnd::CommandOutcome blinded = server.HandleCommand(
      dnd::AttackToken{.attacker = 1, .target = 2}, "alice", false);
  Expect(blinded.accepted, "a blinded character still attacks");
  Expect(std::ranges::any_of(
             blinded.events,
             [](const dnd::Event& event) -> bool {
               if (!std::holds_alternative<dnd::LogLineAdded>(event)) {
                 return false;
               }
               return std::get<dnd::LogLineAdded>(event).line.text.find(
                          "с помехой") != std::string::npos;
             }),
         "condition disadvantage is written to the log");

  // Встать из «сбит с ног» стоит половину скорости, двигаться нельзя
  // захваченному.
  const dnd::GameSnapshot prone_snapshot{
      .map = dnd::GridMap(10, 1),
      .tokens = {dnd::Token{.id = 1,
                            .name = "hero",
                            .owner = "alice",
                            .hp = 10,
                            .max_hp = 10,
                            .speed = 9,
                            .conditions = {dnd::Condition{"сбит с ног"}},
                            .kind = dnd::TokenKind::kPlayer,
                            .cell = CellAt(0, 0)}},
      .next_token_id = 2,
      .encounter = dnd::Encounter{.active = true, .order = {1}},
  };
  dnd::GameServer prone_server(prone_snapshot, 6, 3);
  dnd::ClientView prone_view;
  prone_view.Reset(prone_server.Snapshot(), prone_server.FullFogState());
  Expect(!prone_server
              .HandleCommand(dnd::MoveToken{.id = 1, .target = CellAt(4, 0)},
                             "alice", false)
              .accepted,
         "standing up spends half of the movement budget");
  Expect(prone_server
             .HandleCommand(dnd::MoveToken{.id = 1, .target = CellAt(2, 0)},
                            "alice", false)
             .accepted,
         "a prone character stands up and moves");
  const dnd::Token stood = TokenOf(prone_server.State(), 1);
  Expect(!HasCondition(stood, "сбит с ног"),
         "standing up removes the prone condition");
  Expect(stood.movement_used == 5,
         "the move charged the standing cost plus the path");

  const dnd::GameSnapshot grappled_snapshot{
      .map = dnd::GridMap(10, 1),
      .tokens = {dnd::Token{.id = 1,
                            .name = "hero",
                            .owner = "alice",
                            .hp = 10,
                            .max_hp = 10,
                            .conditions = {dnd::Condition{"захвачен"}},
                            .kind = dnd::TokenKind::kPlayer,
                            .cell = CellAt(0, 0)}},
      .next_token_id = 2,
      .encounter = dnd::Encounter{.active = true, .order = {1}},
  };
  dnd::GameServer grappled_server(grappled_snapshot, 6, 3);
  Expect(!grappled_server
              .HandleCommand(dnd::MoveToken{.id = 1, .target = CellAt(1, 0)},
                             "alice", false)
              .accepted,
         "a grappled character cannot move");
}

auto TestDamageTypesAndDefenses() -> void {
  using dnd::DamageType;
  using dnd::DamageTypeBit;
  const dnd::DamageProfile profile{
      .resist = DamageTypeBit(DamageType::kFire),
      .vulnerable = DamageTypeBit(DamageType::kCold),
      .immune = DamageTypeBit(DamageType::kPoison),
  };
  Expect(dnd::AdjustedDamage(11, DamageType::kFire, profile) == 5,
         "resistance halves damage, rounding down");
  Expect(dnd::AdjustedDamage(11, DamageType::kCold, profile) == 22,
         "vulnerability doubles damage");
  Expect(dnd::AdjustedDamage(11, DamageType::kPoison, profile) == 0,
         "immunity removes damage");
  Expect(dnd::AdjustedDamage(11, DamageType::kSlashing, profile) == 11,
         "untyped defence leaves damage alone");
  const dnd::DamageProfile both{
      .resist = DamageTypeBit(DamageType::kFire),
      .vulnerable = DamageTypeBit(DamageType::kFire),
  };
  Expect(dnd::AdjustedDamage(25, DamageType::kFire, both) == 24,
         "resistance halves first, vulnerability doubles after");

  dnd::DiceExpression expression;
  DamageType type = DamageType::kNone;
  Expect(dnd::ParseDamageProfile("1d8+2 огонь", expression, type) &&
             type == DamageType::kFire && expression.modifier == 2,
         "damage sheet holds an expression and a type");
  Expect(dnd::IsDamageProfilePrefix("1d8+2 ого"),
         "a partially typed damage type is accepted");
  Expect(!dnd::IsDamageProfilePrefix("1d8+2 огоx"),
         "an unknown damage type is rejected");
  std::uint32_t mask = 0;
  Expect(!dnd::ParseDamageTypeList("огонь, холод и", mask),
         "an unknown name rejects the whole list");
  Expect(dnd::ParseDamageTypeList("огонь, холод", mask) &&
             dnd::FormatDamageTypeList(mask) == "холод, огонь",
         "damage type lists round-trip");

  const dnd::GridMap map(8, 8);
  const dnd::GameSnapshot snapshot{
      .map = map,
      .tokens =
          {
              dnd::Token{.id = 1,
                         .name = "hero",
                         .owner = "alice",
                         .hp = 40,
                         .max_hp = 40,
                         .damage = "1d4 огонь",
                         .resist = DamageTypeBit(DamageType::kFire),
                         .kind = dnd::TokenKind::kPlayer,
                         .cell = CellAt(1, 1)},
              dnd::Token{.id = 2,
                         .name = "skeleton",
                         .owner = "dm",
                         .hp = 20,
                         .max_hp = 20,
                         .vulnerable = DamageTypeBit(DamageType::kFire),
                         .kind = dnd::TokenKind::kMonster,
                         .cell = CellAt(2, 1)},
              dnd::Token{.id = 3,
                         .name = "statue",
                         .owner = "dm",
                         .hp = 20,
                         .max_hp = 20,
                         .conditions = {dnd::Condition{"окаменел"}},
                         .kind = dnd::TokenKind::kMonster,
                         .cell = CellAt(4, 4)},
          },
      .next_token_id = 4,
  };
  dnd::GameServer server(snapshot, 6, 7);
  const dnd::CommandOutcome resisted = server.HandleCommand(
      dnd::DamageToken{.id = 1, .amount = 10, .damage_type = DamageType::kFire},
      "alice", false);
  Expect(resisted.accepted && TokenOf(server.State(), 1).hp == 35,
         "fire resistance halves the damage");
  const dnd::CommandOutcome vulnerable = server.HandleCommand(
      dnd::DamageToken{.id = 2, .amount = 10, .damage_type = DamageType::kFire},
      "", true);
  Expect(vulnerable.accepted && TokenOf(server.State(), 2).hp == 0,
         "a vulnerable target takes doubled fire damage");
  const dnd::CommandOutcome statue_poison = server.HandleCommand(
      dnd::DamageToken{
          .id = 3, .amount = 10, .damage_type = DamageType::kPoison},
      "", true);
  Expect(statue_poison.accepted && TokenOf(server.State(), 3).hp == 20,
         "a petrified target is immune to poison");
  Expect(std::ranges::any_of(
             statue_poison.events,
             [](const dnd::Event& event) -> bool {
               if (!std::holds_alternative<dnd::LogLineAdded>(event)) {
                 return false;
               }
               return std::get<dnd::LogLineAdded>(event).line.text.find(
                          "иммунитет") != std::string::npos;
             }),
         "immunity is written to the log");
  const dnd::CommandOutcome statue_fire = server.HandleCommand(
      dnd::DamageToken{.id = 3, .amount = 10, .damage_type = DamageType::kFire},
      "", true);
  Expect(statue_fire.accepted && TokenOf(server.State(), 3).hp == 15,
         "a petrified target resists all typed damage");

  // Временные HP не складываются: остаётся большее значение.
  const auto sheet = [&server](int temp_hp) -> dnd::CommandOutcome {
    return server.HandleCommand(
        dnd::UpdateTokenSheet{.id = 1,
                              .ac = 10,
                              .speed = 9,
                              .notes = "",
                              .attack_bonus = 0,
                              .save_bonus = 0,
                              .initiative_bonus = 0,
                              .damage = "1d4 огонь",
                              .temp_hp = temp_hp,
                              .hit_die = 8,
                              .con_modifier = 0,
                              .attack_range = 0,
                              .resist = DamageTypeBit(DamageType::kFire)},
        "", true);
  };
  Expect(sheet(5).accepted && TokenOf(server.State(), 1).temp_hp == 5,
         "temporary HP can be granted");
  Expect(sheet(3).accepted && TokenOf(server.State(), 1).temp_hp == 5,
         "a smaller amount of temporary HP does not replace the pool");
  Expect(sheet(8).accepted && TokenOf(server.State(), 1).temp_hp == 8,
         "a bigger amount of temporary HP replaces the pool");

  // Атака с типом урона сообщает сопротивление в логе.
  const dnd::GameSnapshot attack_snapshot{
      .map = map,
      .tokens = {dnd::Token{.id = 1,
                            .name = "mage",
                            .owner = "alice",
                            .hp = 10,
                            .max_hp = 10,
                            .attack_bonus = 100,
                            .damage = "1d4 холод",
                            .kind = dnd::TokenKind::kPlayer,
                            .cell = CellAt(1, 1)},
                 dnd::Token{.id = 2,
                            .name = "yeti",
                            .owner = "dm",
                            .hp = 1000,
                            .max_hp = 1000,
                            .ac = 10,
                            .resist = DamageTypeBit(DamageType::kCold),
                            .kind = dnd::TokenKind::kMonster,
                            .cell = CellAt(2, 1)}},
      .next_token_id = 3,
  };
  dnd::GameServer attack_server(attack_snapshot, 6, 11);
  bool saw_resistance = false;
  for (int i = 0; i < 20 && !saw_resistance; ++i) {
    const dnd::CommandOutcome outcome = attack_server.HandleCommand(
        dnd::AttackToken{.attacker = 1, .target = 2}, "alice", false);
    Expect(outcome.accepted, "a typed attack is accepted");
    for (const dnd::Event& event : outcome.events) {
      if (const auto* line = std::get_if<dnd::LogLineAdded>(&event)) {
        saw_resistance =
            saw_resistance ||
            line->line.text.find("сопротивление") != std::string::npos;
      }
    }
  }
  Expect(saw_resistance, "an attack with a damage type respects resistance");
}

auto TestRangedAttackAndSurprise() -> void {
  // Клетки: стрелок, соседний союзник и цель на дистанции.
  const dnd::GridMap map(10, 1);
  const auto make_snapshot = [&map](int target_x) -> dnd::GameSnapshot {
    return dnd::GameSnapshot{
        .map = map,
        .tokens =
            {
                dnd::Token{.id = 1,
                           .name = "archer",
                           .owner = "alice",
                           .hp = 10,
                           .max_hp = 10,
                           .ac = 10,
                           .attack_bonus = 100,
                           .damage = "1d4",
                           .attack_range = 2,
                           .kind = dnd::TokenKind::kPlayer,
                           .cell = CellAt(0, 0)},
                dnd::Token{.id = 2,
                           .name = "buddy",
                           .owner = "alice",
                           .hp = 10,
                           .max_hp = 10,
                           .kind = dnd::TokenKind::kPlayer,
                           .cell = CellAt(1, 0)},
                dnd::Token{.id = 3,
                           .name = "goblin",
                           .owner = "dm",
                           .hp = 1000,
                           .max_hp = 1000,
                           .ac = 10,
                           .kind = dnd::TokenKind::kMonster,
                           .cell = CellAt(target_x, 0)},
            },
        .next_token_id = 4,
    };
  };
  dnd::GameServer out_of_range(make_snapshot(5), 6, 21);
  const dnd::CommandOutcome long_shot = out_of_range.HandleCommand(
      dnd::AttackToken{.attacker = 1, .target = 3}, "alice", false);
  Expect(!long_shot.accepted &&
             long_shot.reject_reason.find("пределами") != std::string::npos,
         "a target out of range is beyond reach");
  dnd::GameServer near_server(make_snapshot(4), 6, 21);
  const dnd::CommandOutcome shot = near_server.HandleCommand(
      dnd::AttackToken{.attacker = 1, .target = 3}, "alice", false);
  Expect(shot.accepted, "an attack at long range is allowed");
  Expect(std::ranges::any_of(
             shot.events,
             [](const dnd::Event& event) -> bool {
               if (!std::holds_alternative<dnd::LogLineAdded>(event)) {
                 return false;
               }
               return std::get<dnd::LogLineAdded>(event).line.text.find(
                          "с помехой") != std::string::npos;
             }),
         "a ranged attack beyond normal range has disadvantage");
  // Цель вплотную, но рядом свой: на дистанции 1 вторая цель тоже сосед.
  dnd::GameServer point_blank(make_snapshot(2), 6, 21);
  const dnd::CommandOutcome close_shot = point_blank.HandleCommand(
      dnd::AttackToken{.attacker = 1, .target = 3}, "alice", false);
  Expect(close_shot.accepted, "a point-blank ranged attack is allowed");
  Expect(std::ranges::any_of(
             close_shot.events,
             [](const dnd::Event& event) -> bool {
               if (!std::holds_alternative<dnd::LogLineAdded>(event)) {
                 return false;
               }
               return std::get<dnd::LogLineAdded>(event).line.text.find(
                          "с помехой") != std::string::npos;
             }),
         "a ranged attack next to another enemy has disadvantage");

  // Застигнутый врасплох бросает инициативу с помехой и теряет состояние.
  const auto initiative_snapshot =
      [&map](std::vector<dnd::Condition> conditions,
             int initiative_bonus) -> dnd::GameSnapshot {
    return dnd::GameSnapshot{
        .map = map,
        .tokens = {dnd::Token{.id = 1,
                              .name = "hero",
                              .owner = "alice",
                              .initiative_bonus = initiative_bonus,
                              .conditions = std::move(conditions),
                              .kind = dnd::TokenKind::kPlayer,
                              .cell = CellAt(0, 0)}},
        .next_token_id = 2,
    };
  };
  dnd::GameServer calm(initiative_snapshot({}, 5), 6, 42);
  dnd::GameServer surprised(
      initiative_snapshot({dnd::Condition{"застигнут врасплох"}}, 5), 6, 42);
  Expect(calm.HandleCommand(dnd::StartEncounter{}, "", true).accepted,
         "the calm encounter starts");
  Expect(surprised.HandleCommand(dnd::StartEncounter{}, "", true).accepted,
         "the surprising encounter starts");
  const int calm_initiative = TokenOf(calm.State(), 1).initiative;
  const int surprised_initiative = TokenOf(surprised.State(), 1).initiative;
  Expect(surprised_initiative <= calm_initiative,
         "surprise rolls initiative with disadvantage");
  Expect(surprised_initiative >= 1 + 5 && surprised_initiative <= 20 + 5,
         "surprise keeps the sheet bonus");
  Expect(!HasCondition(TokenOf(surprised.State(), 1), "застигнут врасплох"),
         "surprise is spent after the initiative roll");

  dnd::GameServer tired(
      initiative_snapshot(
          {dnd::Condition{"истощение"}, dnd::Condition{"истощение"}}, 5),
      6, 42);
  Expect(tired.HandleCommand(dnd::StartEncounter{}, "", true).accepted,
         "the tired encounter starts");
  Expect(TokenOf(tired.State(), 1).initiative == calm_initiative - 4,
         "exhaustion subtracts two from initiative per level");
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
  Expect(bestiary.size() == 9, "bestiary has nine templates");
  Expect(!bestiary.empty(), "bestiary is not empty");
  if (!bestiary.empty()) {
    Expect(!bestiary.front().name.empty(), "bestiary entries have names");
    Expect(bestiary.front().ac > 0, "у записей бестиария есть КД");
    Expect(bestiary.front().max_hp > 0, "bestiary entries have HP");
    Expect(!bestiary.front().damage.empty(),
           "bestiary entries have damage expressions");
  }
  for (const dnd::MonsterTemplate& monster : bestiary) {
    Expect(!monster.name.empty() && monster.ac > 0 && monster.max_hp > 0 &&
               !monster.damage.empty() && !monster.notes.empty(),
           "every bestiary template is complete");
  }
  Expect(dnd::DefaultConditions().size() >= 8,
         "default conditions are available");

  // Тесту нужен повторяемый поток, а не криптостойкость.
  // NOLINTNEXTLINE(bugprone-random-generator-seed)
  std::mt19937 rng(7);
  bool saw_minimum = false;
  bool saw_maximum = false;
  for (int i = 0; i < 200; ++i) {
    const int size = dnd::MonsterGroupSize(rng);
    Expect(size >= dnd::kMinMonsterGroup && size <= dnd::kMaxMonsterGroup,
           "group size stays within 3..5");
    saw_minimum = saw_minimum || size == dnd::kMinMonsterGroup;
    saw_maximum = saw_maximum || size == dnd::kMaxMonsterGroup;
  }
  Expect(saw_minimum && saw_maximum, "group size covers the whole range");

  dnd::GridMap map(6, 6);
  map.SetBlocked(CellAt(1, 1), true);
  const std::vector<dnd::Token> tokens{
      dnd::Token{.id = 1, .name = "hero", .cell = CellAt(3, 3)}};
  const std::vector<dnd::Cell> cells =
      dnd::MonsterGroupCells(map, CellAt(3, 3), 3, tokens);
  Expect(cells.empty(), "a group cannot start on an occupied cell");

  const std::vector<dnd::Cell> around =
      dnd::MonsterGroupCells(map, CellAt(2, 2), 4, tokens);
  Expect(around.size() == 4 && around.front() == CellAt(2, 2),
         "the group starts at the cursor and fills the neighbourhood");
  Expect(std::ranges::find(around, CellAt(1, 1)) == around.end(),
         "walls are skipped when placing a group");
  Expect(std::ranges::find(around, CellAt(3, 3)) == around.end(),
         "tokens are skipped when placing a group");

  const std::vector<dnd::Cell> squeezed =
      dnd::MonsterGroupCells(dnd::GridMap(1, 1), CellAt(0, 0), 5, {});
  Expect(squeezed.size() == 1, "a tiny map fits only the cursor cell");
  Expect(dnd::MonsterGroupCells(map, CellAt(9, 9), 3, {}).empty(),
         "an off-map origin places nothing");
}

auto TestDynamicFogObservers() -> void {
  dnd::GridMap map(12, 1);
  map.SetBlocked(CellAt(5, 0), true);
  const dnd::GameSnapshot snapshot{
      .map = map,
      .tokens = {dnd::Token{.id = 1,
                            .name = "hero",
                            .owner = "alice",
                            .kind = dnd::TokenKind::kPlayer,
                            .cell = CellAt(0, 0)}},
      .next_token_id = 2,
  };
  dnd::GameServer server(snapshot, 2, 2);
  Expect(!FogVisibleAt(server.FogState("alice"), 10, 0),
         "far cells start outside the player's vision");

  const dnd::CommandOutcome added =
      server.HandleCommand(dnd::AddToken{.name = "ally",
                                         .cell = CellAt(10, 0),
                                         .owner = "bob",
                                         .kind = dnd::TokenKind::kPlayer,
                                         .max_hp = 10},
                           "", true);
  Expect(added.accepted, "the DM can add a player character");
  Expect(FogVisibleAt(server.FogState("bob"), 10, 0),
         "a new character gives its player vision");
  Expect(!FogVisibleAt(server.FogState("alice"), 10, 0),
         "another player does not get vision from it");

  const dnd::CommandOutcome removed =
      server.HandleCommand(dnd::RemoveToken{.id = 2}, "", true);
  Expect(removed.accepted, "the DM can remove the character");
  Expect(!FogVisibleAt(server.FogState("bob"), 10, 0),
         "removing a character removes its vision");
  Expect(FogExploredAt(server.FogState("bob"), 10, 0),
         "the cell stays explored after the character leaves");
}

// Туман живёт в сцене: отмена, загрузка и смена карты его не обнуляют.
auto TestFogSurvivesReset() -> void {
  const dnd::GridMap map(12, 1);
  const dnd::GameSnapshot snapshot{
      .map = map,
      .tokens = {dnd::Token{.id = 1,
                            .name = "hero",
                            .owner = "alice",
                            .kind = dnd::TokenKind::kPlayer,
                            .cell = CellAt(0, 0)}},
      .next_token_id = 2,
  };
  dnd::GameServer server(snapshot, 2, 4);
  Expect(FogExploredAt(server.FogState("alice"), 1, 0),
         "the starting area is explored");
  Expect(!FogExploredAt(server.FogState("alice"), 10, 0),
         "far cells start unexplored");

  const dnd::GameSnapshot before = server.Snapshot();
  Expect(before.fog.size() == 2,
         "the scene keeps the player fog and the party fog");
  Expect(server
             .HandleCommand(dnd::MoveToken{.id = 1, .target = CellAt(10, 0)},
                            "", true)
             .accepted,
         "the character moves across the map");
  Expect(FogExploredAt(server.FogState("alice"), 10, 0),
         "the destination becomes explored");

  // Отмена (Ctrl+Z): мир возвращается вместе с туманом того момента.
  server.Reset(before);
  Expect(FogExploredAt(server.FogState("alice"), 1, 0),
         "undo keeps the cells explored before the action");
  Expect(!FogExploredAt(server.FogState("alice"), 10, 0),
         "undo forgets cells explored by the undone action");

  // Новый сервер из снимка — это загрузка сохранения или смена сцены.
  dnd::GameServer reopened(server.Snapshot(), 2, 4);
  Expect(FogExploredAt(reopened.FogState("alice"), 1, 0),
         "a saved scene restores the explored cells");
  Expect(FogVisibleAt(reopened.FogState("alice"), 0, 0),
         "current vision is recomputed after loading");
  Expect(reopened.ClientSnapshot("alice").fog.empty(),
         "the client snapshot carries no fog records");

  // Карта другой формы: исследованное некуда переносить.
  const dnd::GameSnapshot other{
      .map = dnd::GridMap(6, 6),
      .tokens = {dnd::Token{.id = 1,
                            .name = "hero",
                            .owner = "alice",
                            .kind = dnd::TokenKind::kPlayer,
                            .cell = CellAt(5, 5)}},
      .next_token_id = 2,
  };
  reopened.Reset(other);
  Expect(!FogExploredAt(reopened.FogState("alice"), 1, 0),
         "a map of another size starts with clean fog");
  Expect(FogExploredAt(reopened.FogState("alice"), 5, 5),
         "the new map still remembers its first sight");
}

auto TestSeparateVisibility() -> void {
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
                         .name = "scout",
                         .owner = "bob",
                         .kind = dnd::TokenKind::kPlayer,
                         .cell = CellAt(10, 0)},
              dnd::Token{.id = 3,
                         .name = "goblin",
                         .owner = "dm",
                         .kind = dnd::TokenKind::kMonster,
                         .cell = CellAt(15, 0)},
          },
      .next_token_id = 4,
  };
  dnd::GameServer server(snapshot, 3, 3);

  const auto snapshot_has = [&server](std::string_view actor,
                                      dnd::TokenId id) -> bool {
    const dnd::GameSnapshot view = server.ClientSnapshot(actor);
    return std::ranges::any_of(
        view.tokens,
        [id](const dnd::Token& token) -> bool { return token.id == id; });
  };
  Expect(snapshot_has("alice", 1) && !snapshot_has("alice", 2),
         "alice sees only her own character");
  Expect(snapshot_has("bob", 2) && !snapshot_has("bob", 1),
         "bob sees only his own character");
  Expect(snapshot_has("carol", 1) && snapshot_has("carol", 2) &&
             !snapshot_has("carol", 3),
         "a player without a character sees the party, not the monsters");

  // Чужой ход не должен утекать: событие видно только владельцу.
  const dnd::CommandOutcome moved = server.HandleCommand(
      dnd::MoveToken{.id = 2, .target = CellAt(11, 0)}, "bob", false);
  Expect(moved.accepted, "bob moves his character");
  for (const dnd::Event& event : moved.events) {
    if (std::holds_alternative<dnd::TokenMoved>(event)) {
      Expect(server.EventVisibleTo(event, "bob"), "bob sees his own move");
      Expect(!server.EventVisibleTo(event, "alice"),
             "alice does not see bob's move");
    }
  }

  // Ручное открытие области DM-ом видят все, даже игрок без персонажа.
  Expect(!server.IsVisibleTo("alice", CellAt(15, 0)),
         "the far corner is hidden before the reveal");
  const dnd::CommandOutcome reveal = server.HandleCommand(
      dnd::RevealArea{.cell = CellAt(15, 0), .radius = 4}, "", true);
  Expect(
      reveal.accepted && reveal.events.size() == 1 &&
          std::holds_alternative<dnd::RevealedUpdated>(reveal.events.front()),
      "the DM reveals an area for the table");
  Expect(server.IsVisibleTo("alice", CellAt(15, 0)) &&
             server.IsVisibleTo("carol", CellAt(15, 0)),
         "revealed cells are visible to every player");
  Expect(snapshot_has("alice", 3), "the goblin appears in the revealed area");
  Expect(FogVisibleAt(server.FogState("carol"), 15, 0),
         "a characterless player receives the reveal in the party fog");
  Expect(
      !server
           .HandleCommand(dnd::RevealArea{.cell = CellAt(15, 0), .radius = 4},
                          "alice", false)
           .accepted,
      "only the DM reveals areas");

  // Смена владельца переносит обзор: старый владелец больше не видит токен.
  Expect(server
             .HandleCommand(dnd::SetTokenOwner{.id = 1, .owner = "carol"}, "",
                            true)
             .accepted,
         "the DM reassigns a character");
  Expect(server.IsVisibleTo("carol", CellAt(0, 0)),
         "the new owner gains vision from the character");
  // Без персонажа alice снова смотрит обзором партии: свой туман больше не
  // закрывает чужие области.
  Expect(FogVisibleAt(server.FogState("alice"), 10, 0),
         "the old owner falls back to the party view");

  const dnd::CommandOutcome hidden = server.HandleCommand(
      dnd::HideArea{.cell = CellAt(18, 0), .radius = 1}, "", true);
  Expect(
      hidden.accepted && hidden.events.size() == 1 &&
          std::holds_alternative<dnd::RevealedUpdated>(hidden.events.front()),
      "the DM hides a part of the revealed area");
  Expect(server.IsVisibleTo("alice", CellAt(15, 0)),
         "revealed cells outside the hidden area stay visible");
  Expect(!server.IsVisibleTo("alice", CellAt(18, 0)),
         "a targeted hide closes only its own circle");
  Expect(!server
              .HandleCommand(dnd::HideArea{.cell = CellAt(18, 0), .radius = 1},
                             "alice", false)
              .accepted,
         "only the DM hides areas");

  const dnd::CommandOutcome cleared =
      server.HandleCommand(dnd::ClearRevealed{}, "", true);
  Expect(cleared.accepted && !server.IsVisibleTo("alice", CellAt(15, 0)),
         "clearing reveals hides the area again");
  Expect(FogExploredAt(server.FogState("alice"), 15, 0),
         "cells seen through a reveal stay explored");
  Expect(!snapshot_has("alice", 3), "the goblin is hidden again");
}

auto TestStaleFogRecord() -> void {
  const dnd::GridMap map(20, 1);
  dnd::GameSnapshot snapshot{
      .map = map,
      .tokens =
          {
              dnd::Token{.id = 1,
                         .name = "hero",
                         .owner = "alice",
                         .kind = dnd::TokenKind::kPlayer,
                         .cell = CellAt(0, 0)},
              dnd::Token{.id = 2,
                         .name = "scout",
                         .kind = dnd::TokenKind::kPlayer,
                         .cell = CellAt(10, 0)},
          },
      .next_token_id = 3,
  };
  // След прошлой партии: у bob есть запись тумана, но персонажа нет.
  snapshot.fog.push_back(dnd::FogRecord{
      .owner = "bob",
      .explored = std::vector<std::uint8_t>(20, 0),
  });
  dnd::GameServer server(snapshot, 3, 7);

  Expect(FogVisibleAt(server.FogState("bob"), 0, 0),
         "a stale fog record does not hide the party view");
  Expect(FogVisibleAt(server.FogState("bob"), 10, 0),
         "a characterless player sees every party character");
  Expect(server.IsVisibleTo("bob", CellAt(10, 0)),
         "the characterless player receives party tokens");

  Expect(
      server
          .HandleCommand(dnd::SetTokenOwner{.id = 2, .owner = "bob"}, "", true)
          .accepted,
      "the DM assigns the character to bob");
  Expect(FogVisibleAt(server.FogState("bob"), 10, 0),
         "the assigned character gives bob his own vision");
  Expect(!FogVisibleAt(server.FogState("bob"), 0, 0),
         "bob no longer sees through alice's character");
  Expect(!FogVisibleAt(server.FogState("alice"), 10, 0),
         "alice keeps only her own vision");
}

auto TestIncrementalView() -> void {
  const dnd::GridMap map(20, 5);
  const dnd::GameSnapshot snapshot{
      .map = map,
      .tokens =
          {
              dnd::Token{.id = 1,
                         .name = "hero",
                         .owner = "alice",
                         .kind = dnd::TokenKind::kPlayer,
                         .cell = CellAt(0, 2)},
              dnd::Token{.id = 2,
                         .name = "goblin",
                         .kind = dnd::TokenKind::kMonster,
                         .cell = CellAt(19, 4)},
          },
      .next_token_id = 3,
  };
  dnd::GameServer server(snapshot, 3, 5);
  dnd::ClientView view;
  view.Reset(server.ClientSnapshot("alice"), server.FogState("alice"));
  // Так хост отмечает только что отправленный Welcome.
  server.MarkViewSent("alice");

  const auto apply_delta =
      [&server, &view](std::string_view actor) -> std::vector<dnd::Event> {
    std::vector<dnd::Event> events = server.TakeViewDelta(actor);
    for (const dnd::Event& event : events) {
      view.Apply(event);
    }
    return events;
  };
  const auto same_fog = [](const dnd::FogSnapshot& left,
                           const dnd::FogSnapshot& right) -> bool {
    return left.width == right.width && left.height == right.height &&
           left.visible == right.visible && left.explored == right.explored &&
           left.light == right.light;
  };

  // Мир не менялся: дельта пустая, полный снимок зря не ходит.
  Expect(apply_delta("alice").empty(), "a quiet world sends no view delta");

  // Монстр входит в поле зрения — приходит только его токен.
  Expect(server
             .HandleCommand(dnd::MoveToken{.id = 2, .target = CellAt(2, 2)}, "",
                            true)
             .accepted,
         "the DM moves the goblin into sight");
  const std::vector<dnd::Event> appeared = apply_delta("alice");
  Expect(appeared.size() == 1 &&
             std::holds_alternative<dnd::TokenAdded>(appeared.front()) &&
             std::get<dnd::TokenAdded>(appeared.front()).token.id == 2,
         "a token entering sight arrives as TokenAdded");
  Expect(!std::ranges::any_of(appeared,
                              [](const dnd::Event& event) -> bool {
                                return std::holds_alternative<dnd::ViewUpdated>(
                                    event);
                              }),
         "the delta never carries the full world");
  Expect(view.Tokens().size() == 2 && view.Fog().IsVisible(CellAt(2, 2)),
         "the client shows the token and its cell");

  // Уход из поля зрения — только удаление, туман не менялся.
  Expect(server
             .HandleCommand(dnd::MoveToken{.id = 2, .target = CellAt(19, 4)},
                            "", true)
             .accepted,
         "the DM moves the goblin away");
  const std::vector<dnd::Event> left = apply_delta("alice");
  Expect(left.size() == 1 &&
             std::holds_alternative<dnd::TokenRemoved>(left.front()) &&
             std::get<dnd::TokenRemoved>(left.front()).id == 2,
         "a hidden token leaves as TokenRemoved");
  Expect(view.Tokens().size() == 1, "the client hides the token again");

  // Ход персонажа: приходят только клетки, где туман реально изменился.
  Expect(server
             .HandleCommand(dnd::MoveToken{.id = 1, .target = CellAt(1, 2)}, "",
                            true)
             .accepted,
         "the character steps aside");
  const std::vector<dnd::Event> walked = apply_delta("alice");
  std::size_t changed_cells = 0;
  for (const dnd::Event& event : walked) {
    if (const auto* fog = std::get_if<dnd::FogChanged>(&event);
        fog != nullptr) {
      changed_cells += fog->cells.size();
    }
  }
  Expect(changed_cells > 0, "movement delivers changed fog cells");
  // Карта 20x5 — сто клеток; сдвиг на одну клетку трогает её край.
  Expect(changed_cells < 20, "only the edge of the fog is resent");
  Expect(same_fog(view.Fog().Snapshot(), server.FogState("alice")),
         "the client fog matches the server after the delta");

  // Ручное открытие DM-а тоже приходит дельтой: туман плюс токен в нём.
  Expect(server
             .HandleCommand(dnd::RevealArea{.cell = CellAt(19, 4), .radius = 1},
                            "", true)
             .accepted,
         "the DM reveals the far corner");
  const std::vector<dnd::Event> revealed = apply_delta("alice");
  Expect(std::ranges::any_of(revealed,
                             [](const dnd::Event& event) -> bool {
                               return std::holds_alternative<dnd::TokenAdded>(
                                   event);
                             }),
         "the revealed token is sent as TokenAdded");
  Expect(std::ranges::any_of(revealed,
                             [](const dnd::Event& event) -> bool {
                               return std::holds_alternative<dnd::FogChanged>(
                                   event);
                             }),
         "the reveal is sent as fog cells");
  Expect(view.Tokens().size() == 2 && view.Fog().IsVisible(CellAt(19, 4)),
         "the client sees the revealed corner");
  Expect(same_fog(view.Fog().Snapshot(), server.FogState("alice")),
         "the reveal keeps both fog copies in sync");

  // Снятие открытий: токен снова скрыт, клетка остаётся разведанной.
  Expect(server.HandleCommand(dnd::ClearRevealed{}, "", true).accepted,
         "the DM clears the reveals");
  apply_delta("alice");
  Expect(view.Tokens().size() == 1, "the token is hidden again");
  Expect(!view.Fog().IsVisible(CellAt(19, 4)) &&
             view.Fog().IsExplored(CellAt(19, 4)),
         "a cell seen through a reveal stays explored on the client");
  Expect(same_fog(view.Fog().Snapshot(), server.FogState("alice")),
         "the cleared reveal keeps both fog copies in sync");

  // Слепок после полной пересылки: следующая дельта снова пустая.
  server.MarkViewSent("alice");
  Expect(apply_delta("alice").empty(),
         "marking the full view resets the delta");
}

auto TestRevealedAreas() -> void {
  dnd::GameState state{dnd::GridMap(10, 10)};
  Expect(state.RevealArea(CellAt(2, 2), 1), "an area reveal changes the list");
  Expect(state.IsRevealed(CellAt(2, 2)) && state.IsRevealed(CellAt(1, 2)) &&
             state.IsRevealed(CellAt(3, 2)),
         "the whole circle becomes revealed");
  Expect(!state.RevealArea(CellAt(2, 2), 1),
         "revealing the same circle twice changes nothing");

  Expect(state.RevealArea(CellAt(7, 7), 1) && state.IsRevealed(CellAt(7, 7)),
         "a second area joins the list");
  Expect(state.HideArea(CellAt(2, 2), 1),
         "a targeted hide removes its own circle");
  Expect(!state.IsRevealed(CellAt(2, 2)) && !state.IsRevealed(CellAt(3, 2)),
         "hidden cells leave the list");
  Expect(state.IsRevealed(CellAt(7, 7)),
         "revealed cells outside the hidden circle survive");
  Expect(!state.HideArea(CellAt(2, 2), 1),
         "hiding already hidden cells changes nothing");

  const std::vector<dnd::Cell>& revealed = state.Revealed();
  const bool sorted = std::ranges::is_sorted(
      revealed, [](dnd::Cell left, dnd::Cell right) -> bool {
        return left.y != right.y ? left.y < right.y : left.x < right.x;
      });
  Expect(sorted && std::ranges::adjacent_find(revealed) == revealed.end(),
         "revealed cells stay sorted and unique");

  Expect(state.ClearRevealed() && state.Revealed().empty(),
         "clearing removes every revealed cell");
  Expect(!state.ClearRevealed(), "clearing an empty list changes nothing");
}

auto TestLevels() -> void {
  Expect(dnd::XpForLevel(1) == 0, "level 1 starts at zero xp");
  Expect(dnd::XpForLevel(2) == 300, "level 2 needs 300 xp");
  Expect(dnd::XpForLevel(20) == 355000, "level 20 needs 355000 xp");
  Expect(dnd::LevelForXp(0) == 1 && dnd::LevelForXp(299) == 1,
         "experience below the threshold keeps level 1");
  Expect(dnd::LevelForXp(300) == 2 && dnd::LevelForXp(899) == 2,
         "level 2 holds until the next threshold");
  Expect(dnd::LevelForXp(900) == 3, "the threshold promotes to level 3");
  Expect(dnd::LevelForXp(-10) == 1, "negative experience is treated as zero");
  Expect(dnd::LevelForXp(dnd::kMaxXp) == dnd::kMaxLevel,
         "experience is capped at the last level");

  Expect(dnd::ProficiencyBonus(1) == 2 && dnd::ProficiencyBonus(4) == 2,
         "levels 1-4 have a +2 proficiency bonus");
  Expect(dnd::ProficiencyBonus(5) == 3 && dnd::ProficiencyBonus(8) == 3,
         "levels 5-8 have a +3 proficiency bonus");
  Expect(dnd::ProficiencyBonus(9) == 4 && dnd::ProficiencyBonus(12) == 4,
         "levels 9-12 have a +4 proficiency bonus");
  Expect(dnd::ProficiencyBonus(13) == 5 && dnd::ProficiencyBonus(16) == 5,
         "levels 13-16 have a +5 proficiency bonus");
  Expect(dnd::ProficiencyBonus(17) == 6 && dnd::ProficiencyBonus(20) == 6,
         "levels 17-20 have a +6 proficiency bonus");
  Expect(dnd::ProficiencyBonus(0) == 2 && dnd::ProficiencyBonus(30) == 6,
         "the proficiency bonus is clamped to the level range");

  Expect(dnd::IsValidHitDie(6) && dnd::IsValidHitDie(8) &&
             dnd::IsValidHitDie(10) && dnd::IsValidHitDie(12),
         "player hit dice are d6, d8, d10 and d12");
  Expect(!dnd::IsValidHitDie(4) && !dnd::IsValidHitDie(20),
         "other hit dice are rejected");
  Expect(dnd::ParseHitDie("d8") == std::optional<int>{8},
         "the sheet accepts the rules notation d8");
  Expect(dnd::ParseHitDie("8") == std::optional<int>{8},
         "a bare number still means the die size");
  Expect(dnd::ParseHitDie("D12") == std::optional<int>{12},
         "the d prefix is case-insensitive");
  Expect(dnd::ParseHitDie(" d10 ") == std::optional<int>{10},
         "surrounding spaces are trimmed");
  Expect(!dnd::ParseHitDie("2d6+12").has_value() &&
             !dnd::ParseHitDie("d7").has_value() &&
             !dnd::ParseHitDie("d").has_value() &&
             !dnd::ParseHitDie("dd8").has_value() &&
             !dnd::ParseHitDie("d 8").has_value() &&
             !dnd::ParseHitDie("").has_value(),
         "a hit die is a single d6, d8, d10 or d12, not a dice expression");
  Expect(dnd::IsHitDiePrefix("") && dnd::IsHitDiePrefix("d") &&
             dnd::IsHitDiePrefix("1") && dnd::IsHitDiePrefix("d1") &&
             dnd::IsHitDiePrefix("10") && dnd::IsHitDiePrefix("d12"),
         "typing accepts partial hit die notation");
  Expect(!dnd::IsHitDiePrefix("2") && !dnd::IsHitDiePrefix("9") &&
             !dnd::IsHitDiePrefix("d7") && !dnd::IsHitDiePrefix("d13"),
         "typing rejects digits that cannot form a valid hit die");
  Expect(dnd::HitDieAverage(6) == 4 && dnd::HitDieAverage(8) == 5 &&
             dnd::HitDieAverage(10) == 6 && dnd::HitDieAverage(12) == 7,
         "2024 average hit points per hit die");
  Expect(dnd::HitPointsPerLevel(8, 3) == 8,
         "a level adds the hit die average plus Constitution");
  Expect(dnd::HitPointsPerLevel(6, -10) == 1,
         "a level always adds at least one hit point");
  Expect(dnd::HitDiceMaximum(1) == 1 && dnd::HitDiceMaximum(7) == 7,
         "a character has one hit die per level");
  Expect(dnd::HitDiceMaximum(0) == 1 && dnd::HitDiceMaximum(30) == 20,
         "the hit dice pool is clamped to the level range");
  Expect(dnd::HitDiceAvailable(5, 2) == 3, "spent hit dice reduce the pool");
  Expect(dnd::HitDiceAvailable(5, 9) == 0 && dnd::HitDiceAvailable(5, -3) == 5,
         "the available pool never goes negative");
  Expect(dnd::HitDieHeal(5, 3) == 8 && dnd::HitDieHeal(2, -5) == 1,
         "a spent hit die heals its roll plus Constitution, at least one");
  Expect(dnd::IsAbilityScoreLevel(4) && dnd::IsAbilityScoreLevel(8) &&
             dnd::IsAbilityScoreLevel(12) && dnd::IsAbilityScoreLevel(16),
         "ability score improvements come at levels 4, 8, 12 and 16");
  Expect(!dnd::IsAbilityScoreLevel(3) && !dnd::IsAbilityScoreLevel(20),
         "other levels grant no ability score improvement");
  Expect(dnd::kEpicBoonLevel == 19, "the epic boon comes at level 19");
}

auto TestExperience() -> void {
  const dnd::GridMap map(12, 1);
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
                         .name = "ally",
                         .owner = "bob",
                         .kind = dnd::TokenKind::kPlayer,
                         .cell = CellAt(1, 0)},
              dnd::Token{.id = 3,
                         .name = "goblin",
                         .owner = "dm",
                         .kind = dnd::TokenKind::kMonster,
                         .cell = CellAt(2, 0)},
          },
      .next_token_id = 4,
  };
  dnd::GameServer server(snapshot, 8, 3);

  Expect(
      !server
           .HandleCommand(dnd::AwardXp{.id = 1, .amount = 300}, "alice", false)
           .accepted,
      "players cannot award experience");
  Expect(!server.HandleCommand(dnd::AwardXp{.id = 3, .amount = 100}, "", true)
              .accepted,
         "monsters do not gain experience");
  Expect(!server.HandleCommand(dnd::AwardXp{.id = 1, .amount = 0}, "", true)
              .accepted,
         "zero experience is rejected");
  Expect(
      !server
           .HandleCommand(dnd::AwardXp{.id = 1, .amount = dnd::kMaxXpAward + 1},
                          "", true)
           .accepted,
      "a single award above the limit is rejected");

  const dnd::CommandOutcome single =
      server.HandleCommand(dnd::AwardXp{.id = 1, .amount = 300}, "dm", true);
  Expect(single.accepted, "the DM awards experience");
  bool updated = false;
  bool promotion = false;
  bool hp_gain = false;
  for (const dnd::Event& event : single.events) {
    if (const auto* token = std::get_if<dnd::TokenUpdated>(&event)) {
      updated = token->token.id == 1 && token->token.xp == 300;
    }
    if (const auto* line = std::get_if<dnd::LogLineAdded>(&event)) {
      promotion = promotion || line->line.text.find("получает 2 уровень") !=
                                   std::string::npos;
      hp_gain = hp_gain ||
                line->line.text.find("+5 к максимуму HP") != std::string::npos;
    }
  }
  Expect(updated, "the award broadcasts the updated character");
  Expect(promotion, "a level-up is written to the log");
  Expect(hp_gain, "the log tells how many hit points the level gave");
  Expect(TokenOf(server.State(), 1).xp == 300 &&
             dnd::LevelForXp(TokenOf(server.State(), 1).xp) == 2,
         "experience and the derived level are stored");
  Expect(TokenOf(server.State(), 1).max_hp == 15 &&
             TokenOf(server.State(), 1).hp == 15,
         "a d8 character with no Constitution gains 5 hit points");

  const dnd::CommandOutcome party =
      server.HandleCommand(dnd::AwardXp{.amount = 50, .party = true}, "", true);
  Expect(party.accepted, "the DM awards experience to the whole party");
  Expect(TokenOf(server.State(), 1).xp == 350 &&
             TokenOf(server.State(), 2).xp == 50,
         "every character receives the party award");
  Expect(TokenOf(server.State(), 3).xp == 0, "monsters get nothing");

  for (int i = 0; i < 11; ++i) {
    Expect(
        server.HandleCommand(dnd::AwardXp{.id = 1, .amount = 100000}, "", true)
            .accepted,
        "large single awards are accepted");
  }
  Expect(TokenOf(server.State(), 1).xp == dnd::kMaxXp,
         "stored experience is capped");
  Expect(dnd::LevelForXp(TokenOf(server.State(), 1).xp) == dnd::kMaxLevel,
         "a capped character is level 20");
  Expect(TokenOf(server.State(), 1).max_hp == 15 + (18 * 5),
         "every level from 2 to 20 adds hit points");
  Expect(TokenOf(server.State(), 1).attack_bonus == 4 &&
             TokenOf(server.State(), 1).save_bonus == 4,
         "proficiency tiers from 2 to 20 raise attack and saves by four");

  // Кость хитов и Телосложение: d12 даёт в среднем 7 HP, модификатор +2 —
  // ещё два. С 1 по 5 уровень это четыре прироста и рубеж мастерства.
  const dnd::GameSnapshot gaining{
      .map = map,
      .tokens =
          {
              dnd::Token{.id = 1,
                         .name = "tank",
                         .owner = "alice",
                         .hp = 12,
                         .max_hp = 20,
                         .hit_die = 12,
                         .con_modifier = 2,
                         .kind = dnd::TokenKind::kPlayer,
                         .cell = CellAt(0, 0)},
              dnd::Token{.id = 2,
                         .name = "downed",
                         .owner = "bob",
                         .hp = 0,
                         .max_hp = 10,
                         .kind = dnd::TokenKind::kPlayer,
                         .cell = CellAt(1, 0)},
          },
      .next_token_id = 3,
  };
  dnd::GameServer gaining_server(gaining, 8, 3);
  const dnd::CommandOutcome jump = gaining_server.HandleCommand(
      dnd::AwardXp{.id = 1, .amount = 6500}, "", true);
  Expect(jump.accepted, "a jump to level 5 is accepted");
  bool mastery_line = false;
  bool ability_line = false;
  for (const dnd::Event& event : jump.events) {
    if (const auto* line = std::get_if<dnd::LogLineAdded>(&event)) {
      mastery_line =
          mastery_line ||
          line->line.text.find("бонус мастерства +3") != std::string::npos;
      ability_line =
          ability_line ||
          line->line.text.find("повышение характеристик") != std::string::npos;
    }
  }
  Expect(mastery_line, "the log names the new proficiency bonus");
  Expect(ability_line, "the log reminds about the ability score improvement");
  const dnd::Token& tank = TokenOf(gaining_server.State(), 1);
  Expect(tank.max_hp == 20 + (4 * 9) && tank.hp == 12 + (4 * 9),
         "hit die average and Constitution raise both hit point pools");
  Expect(tank.attack_bonus == 1 && tank.save_bonus == 1,
         "the new proficiency tier raises attack and saves");
  Expect(TokenOf(gaining_server.State(), 2).hp == 0 &&
             TokenOf(gaining_server.State(), 2).max_hp == 10,
         "experience alone does not touch other characters");

  const dnd::CommandOutcome downed = gaining_server.HandleCommand(
      dnd::AwardXp{.id = 2, .amount = 300}, "", true);
  Expect(downed.accepted, "a downed character still gains experience");
  Expect(TokenOf(gaining_server.State(), 2).max_hp == 15 &&
             TokenOf(gaining_server.State(), 2).hp == 0,
         "a downed character keeps zero current hit points");
}

auto TestFogMovement() -> void {
  const dnd::GridMap map(20, 1);
  const dnd::GameSnapshot snapshot{
      .map = map,
      .tokens = {dnd::Token{.id = 1,
                            .name = "hero",
                            .owner = "alice",
                            .kind = dnd::TokenKind::kPlayer,
                            .cell = CellAt(0, 0)}},
      .next_token_id = 2,
  };
  dnd::GameServer server(snapshot, 3, 9);

  const auto move = [&server](dnd::Cell target,
                              bool dm = false) -> dnd::CommandOutcome {
    return server.HandleCommand(dnd::MoveToken{.id = 1, .target = target},
                                dm ? "" : "alice", dm);
  };

  const dnd::CommandOutcome blind = move(CellAt(8, 0));
  Expect(!blind.accepted && blind.reject_reason == "клетка не разведана",
         "a player cannot move into an unexplored cell");
  Expect(move(CellAt(3, 0)).accepted, "moving into explored cells is allowed");
  Expect(move(CellAt(0, 0)).accepted, "the character can walk back");
  Expect(move(CellAt(6, 0)).accepted,
         "a previously explored but hidden cell is a valid target");
  const dnd::CommandOutcome far = move(CellAt(15, 0));
  Expect(!far.accepted && far.reject_reason == "клетка не разведана",
         "the far unexplored cell stays blocked");
  Expect(move(CellAt(15, 0), true).accepted, "the DM may move a token blindly");
  const dnd::CommandOutcome gap = move(CellAt(10, 0));
  Expect(!gap.accepted && gap.reject_reason == "клетка не разведана",
         "cells never seen stay closed even after the character moved away");
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

  std::string appended = "Пр";
  dnd::AppendUtf8(appended, "ивет", 5);
  Expect(appended == "Пр", "appending respects the byte limit");
  dnd::AppendUtf8(appended, "и", 6);
  Expect(appended == "При",
         "appending fits a code point that ends on the limit");

  std::string erased = "Привет";
  dnd::EraseLastCodePoint(erased);
  Expect(erased == "Приве", "erase drops the last Cyrillic letter");
  dnd::EraseLastCodePoint(erased);
  Expect(erased == "Прив", "erase works on a shorter string");
  std::string empty;
  dnd::EraseLastCodePoint(empty);
  Expect(empty.empty(), "erase leaves an empty string empty");
}

auto TestPlayerNames() -> void {
  Expect(dnd::PlayerNameKey("Player") == "player",
         "ASCII names fold to lowercase");
  Expect(dnd::PlayerNameKey("ИГРОК") == dnd::PlayerNameKey("игрок"),
         "Cyrillic names fold to lowercase");
  Expect(dnd::PlayerNameKey("ЁЖ") == dnd::PlayerNameKey("ёж"),
         "Cyrillic Ё folds to lowercase");
  Expect(dnd::PlayerNameKey("ÀÉÎ") == dnd::PlayerNameKey("àéî"),
         "Latin-1 names fold to lowercase");
  Expect(dnd::PlayerNameKey("Łukasz") == dnd::PlayerNameKey("łukasz"),
         "Latin Extended-A names fold to lowercase");
  Expect(dnd::PlayerNameKey("ΣΟΦΟΣ") == dnd::PlayerNameKey("σοφος"),
         "Greek names fold, including the final sigma");
  Expect(dnd::SamePlayerName("Player", "PLAYER"), "names compare without case");
  Expect(dnd::SamePlayerName("Игрок", "иГРОК"),
         "Cyrillic names compare without case");
  Expect(!dnd::SamePlayerName("player", "player2"),
         "different names stay different");
  Expect(!dnd::SamePlayerName("player", "player "),
         "trailing spaces still matter");
  Expect(!dnd::SamePlayerName("", "dm"), "an empty name differs from a name");
  Expect(dnd::SamePlayerName("", ""), "empty names are equal");
  Expect(dnd::PlayerNameKey("DM") == "dm", "the reserved name folds too");
}

auto TestVersion() -> void {
  dnd::Version version;
  const auto parse = [&version](std::string_view text) -> bool {
    return dnd::ParseVersion(text, version);
  };

  Expect(parse("0.5.2") && version.major == 0 && version.minor == 5 &&
             version.patch == 2 && !version.dirty,
         "a plain version parses as a release");
  Expect(parse("v0.5.2") && dnd::FormatVersion(version) == "0.5.2",
         "a tag with the v prefix parses the same");
  Expect(parse("0.5.2-3-gabc123") && version.major == 0 && version.minor == 5 &&
             version.patch == 2 && version.dirty,
         "commits past the tag make a developer build");
  Expect(parse("0.5.2-3-gabc123-dirty") && version.dirty,
         "a dirty describe is a developer build");
  Expect(parse("0.0.0-dirty") && version.dirty,
         "the no-git fallback is a developer build");

  const dnd::Version before = version;
  Expect(!parse("") && !parse("0.5") && !parse("0.5.2.1") && !parse("x0.5.2") &&
             !parse("0.5.-2") && !parse("0.5.2-") && !parse(" 0.5.2") &&
             !parse("99999999999.0.0"),
         "malformed versions are rejected");
  Expect(version.major == before.major && version.minor == before.minor &&
             version.patch == before.patch && version.dirty == before.dirty,
         "a rejected version leaves the output untouched");

  Expect(parse("1.2.3"), "version for comparisons parses");
  const dnd::Version older{.major = 1, .minor = 2, .patch = 3};
  const dnd::Version newer_patch{.major = 1, .minor = 2, .patch = 4};
  const dnd::Version newer_minor{.major = 1, .minor = 3, .patch = 0};
  const dnd::Version newer_major{.major = 2, .minor = 0, .patch = 0};
  const dnd::Version dirty_same{
      .major = 1, .minor = 2, .patch = 3, .dirty = true};
  Expect(dnd::CompareVersion(older, older) == 0 &&
             dnd::CompareVersion(older, newer_patch) == -1 &&
             dnd::CompareVersion(newer_patch, older) == 1,
         "versions compare by patch number");
  Expect(dnd::CompareVersion(older, newer_minor) == -1 &&
             dnd::CompareVersion(newer_minor, newer_major) == -1,
         "minor and major outrank a patch difference");
  Expect(dnd::CompareVersion(older, dirty_same) == 0,
         "dirty does not change the numeric order");
  Expect(dnd::FormatVersion(dirty_same) == "1.2.3-dirty",
         "a developer build is formatted with -dirty");
}

auto TestUpdateManifest() -> void {
  const std::string json = R"json( {
  "version": "v0.3.0",
  "protocol": 42,
  "url": "https://example.com/v0.3.0/virtual_dnd_tabletop.exe",
  "sha256": "0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF",
  "notes": "строка\nс \u00e9 и \"кавычками\"",
  "published": "2026-11-22",
  "unknown": {"nested": [1, true, null, "x"]}
 } )json";
  dnd::UpdateManifest manifest;
  Expect(dnd::ParseUpdateManifest(json, manifest),
         "a full manifest with unknown fields parses");
  Expect(manifest.version.major == 0 && manifest.version.minor == 3 &&
             manifest.version.patch == 0,
         "the manifest version is parsed");
  Expect(manifest.protocol == 42, "the protocol number is parsed");
  Expect(manifest.sha256 ==
             "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
         "sha256 is normalized to lower case");
  Expect(manifest.notes == "строка\nс é и \"кавычками\"",
         "JSON escapes and unicode are decoded");
  Expect(manifest.published == "2026-11-22", "published is kept as text");

  dnd::UpdateManifest unchanged = manifest;
  const auto rejects = [&unchanged](std::string_view text) -> bool {
    dnd::UpdateManifest candidate = unchanged;
    return !dnd::ParseUpdateManifest(text, candidate) &&
           candidate.url == unchanged.url &&
           candidate.sha256 == unchanged.sha256 &&
           candidate.version.major == unchanged.version.major;
  };
  Expect(rejects(R"json({"version":"0.2.0"})json"),
         "a manifest without url and sha256 is rejected");
  Expect(rejects(R"json({"version":"0.2.0","url":"x","sha256":"0123"})json"),
         "a short sha256 is rejected");
  Expect(rejects(R"json({"version":"0.2.0","url":"x","sha256":
      "zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz"})json"),
         "a non-hex sha256 is rejected");
  Expect(rejects(R"json({"version":"0.2.0","url":"x","sha256":
          "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",})json"),
         "trailing commas are rejected");
  Expect(rejects(R"json({"version":"0.2.0","url":"x","sha256":
          "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"} garbage)json"),
         "trailing garbage is rejected");
  Expect(rejects(R"json({"version":nope})json"), "broken JSON is rejected");

  const dnd::Version current{.major = 0, .minor = 2, .patch = 0};
  dnd::UpdateManifest newer;
  Expect(dnd::ParseUpdateManifest(
             R"json({"version":"0.2.1","url":"x","sha256":
              "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"})json",
             newer),
         "a minimal manifest parses");
  Expect(dnd::IsUpdateAvailable(current, newer), "a newer manifest is offered");
  Expect(!dnd::IsUpdateAvailable(newer.version, newer),
         "the same version is not offered again");
  Expect(!dnd::IsUpdateAvailable(dnd::Version{.major = 1}, newer),
         "an older manifest is not offered");
  dnd::Version dirty = current;
  dirty.dirty = true;
  Expect(!dnd::IsUpdateAvailable(dirty, newer),
         "a developer build never offers updates");
  Expect(dnd::IsUpdateAvailable(dirty, newer, true),
         "DND_UPDATE_MANIFEST lets a developer build test the cycle");
  Expect(newer.files.empty(), "a manifest without files updates the exe only");

  // Ассеты: путь только внутри assets/, поля обязательны, повторы и опасные
  // пути отвергают манифест целиком — половина файлов хуже, чем ничего.
  const std::string sha(64, 'a');
  const auto entry = [&sha](std::string_view path, std::string_view url,
                            std::string_view size) -> std::string {
    return std::string(R"json({"path":")json") + std::string(path) +
           R"json(","url":")json" + std::string(url) +
           R"json(","sha256":")json" + sha + R"json(","size":)json" +
           std::string(size) + R"json(,"future":{"nested":[1,true]}})json";
  };
  const auto manifest_with = [&sha](std::string_view files) -> std::string {
    return std::string(
               R"json({"version":"0.3.0","url":"x.exe","sha256":")json") +
           sha + R"json(","files":)json" + std::string(files) + "}";
  };
  const auto rejects_files = [&manifest_with](std::string_view files) -> bool {
    dnd::UpdateManifest candidate;
    return !dnd::ParseUpdateManifest(manifest_with(files), candidate);
  };
  const auto many_files = [&entry](int count) -> std::string {
    std::string text = "[";
    for (int i = 0; i < count; ++i) {
      if (i > 0) {
        text += ",";
      }
      text += entry("assets/f" + std::to_string(i) + ".bin", "f", "1");
    }
    text += "]";
    return text;
  };

  dnd::UpdateManifest with_files;
  Expect(dnd::ParseUpdateManifest(
             manifest_with(
                 "[" + entry("assets/fonts/DejaVuSans.ttf", "f.ttf", "1024") +
                 "]"),
             with_files),
         "a files array parses");
  Expect(with_files.files.size() == 1 &&
             with_files.files[0].path == "assets/fonts/DejaVuSans.ttf" &&
             with_files.files[0].url == "f.ttf" &&
             with_files.files[0].sha256 == sha &&
             with_files.files[0].size == 1024,
         "an asset keeps path, url, sha256 and size");
  Expect(rejects_files("[" + entry("assets/../exe.exe", "f", "1") + "]"),
         "a parent segment is rejected");
  Expect(rejects_files("[" + entry("virtual_dnd_tabletop.exe", "f", "1") + "]"),
         "a path outside assets/ is rejected");
  Expect(
      rejects_files("[" + entry("assets/fonts/x.ttf.update", "f", "1") + "]"),
      "the staging suffix is reserved");
  Expect(rejects_files("[" + entry("assets//x.ttf", "f", "1") + "]"),
         "an empty segment is rejected");
  Expect(rejects_files("[" + entry("assets/x.ttf", "f", "0") + "]"),
         "a zero size is rejected");
  Expect(rejects_files("[" + entry("assets/X.ttf", "f", "1") + "," +
                       entry("assets/x.ttf", "f", "1") + "]"),
         "paths differing only by case are one file");
  Expect(rejects_files(
             std::string(R"json([{"path":"assets/x.ttf","sha256":")json") +
             sha + R"json(","size":1}])json"),
         "an asset without a url is rejected");
  Expect(rejects_files(many_files(static_cast<int>(dnd::kMaxUpdateFiles) + 1)),
         "more assets than the limit is rejected");
  Expect(!rejects_files(many_files(static_cast<int>(dnd::kMaxUpdateFiles))),
         "the asset limit itself is allowed");
  Expect(rejects_files(R"json({"assets":"not an array"})json"),
         "files must be an array");

  Expect(dnd::IsSafeUpdatePath("assets/fonts/DejaVuSans.ttf"),
         "a plain asset path is safe");
  Expect(!dnd::IsSafeUpdatePath("assets/x:y"), "a colon is not allowed");
  Expect(!dnd::IsSafeUpdatePath("assets/x."), "a trailing dot is not allowed");
  Expect(!dnd::IsSafeUpdatePath("/assets/x"), "an absolute path is rejected");
  Expect(!dnd::IsSafeUpdatePath("assets\\x"), "a backslash is rejected");
  Expect(!dnd::IsSafeUpdatePath("assets/"), "a bare directory is rejected");
}

auto TestUpdateAssetPlan() -> void {
  const std::filesystem::path directory =
      std::filesystem::temp_directory_path() / "dnd_core_tests_assets";
  std::error_code code;
  std::filesystem::remove_all(directory, code);
  std::filesystem::create_directories(directory / "assets/fonts");

  const auto write = [&directory](std::string_view name,
                                  std::string_view text) -> void {
    std::ofstream file(directory / std::string(name),
                       std::ios::binary | std::ios::trunc);
    file.write(text.data(), static_cast<std::streamsize>(text.size()));
  };
  const auto exists = [&directory](std::string_view name) -> bool {
    std::error_code exists_code;
    return std::filesystem::exists(directory / std::string(name), exists_code);
  };
  const auto hex = [](std::string_view text) -> std::string {
    return dnd::HexDigest(dnd::Sha256(text));
  };
  const auto make = [&hex](std::string path,
                           std::string_view text) -> dnd::UpdateFile {
    dnd::UpdateFile file;
    file.path = std::move(path);
    file.url = "https://example.com/" + file.path;
    file.sha256 = hex(text);
    file.size = text.size();
    return file;
  };

  write("assets/fonts/DejaVuSans.ttf", "font");
  write("assets/fonts/Old.ttf", "old");
  const dnd::UpdateFile font = make("assets/fonts/DejaVuSans.ttf", "font");
  const dnd::UpdateFile missing = make("assets/fonts/Extra.ttf", "extra");
  const dnd::UpdateFile changed = make("assets/fonts/Old.ttf", "new");

  dnd::UpdateDownloadPlan plan;
  dnd::PlanAssetDownloads(directory, {font, missing, changed}, plan);
  Expect(plan.files.size() == 2 && plan.files[0].path == missing.path &&
             plan.files[1].path == changed.path,
         "only missing or changed assets are planned");
  Expect(plan.total_bytes == missing.size + changed.size,
         "the plan sums the bytes it will fetch");

  dnd::PlanAssetDownloads(directory, {font}, plan);
  Expect(plan.files.empty() && plan.total_bytes == 0,
         "a matching asset is not downloaded again");

  // Скачанное лежит рядом с местом назначения и убирается на старте.
  const std::filesystem::path staged =
      dnd::StagedUpdatePath(directory, font.path);
  Expect(staged == directory / "assets/fonts/DejaVuSans.ttf.update",
         "the staged copy sits next to its target");
  write("assets/fonts/DejaVuSans.ttf.update", "half");
  dnd::RemoveStagedAssets(directory);
  Expect(!exists("assets/fonts/DejaVuSans.ttf.update") &&
             exists("assets/fonts/DejaVuSans.ttf"),
         "staged copies are swept and real assets stay");

  std::filesystem::remove_all(directory, code);
}

auto TestUpdateAdvice() -> void {
  const auto make = [](int minor) -> dnd::Version {
    return dnd::Version{.major = 0, .minor = minor, .patch = 0};
  };
  using dnd::UpdateAdvice;
  const dnd::Version older = make(35);
  const dnd::Version current = make(36);
  const dnd::Version newer = make(37);

  Expect(
      dnd::AdviseUpdate(older, current, current) == UpdateAdvice::kUpdateMine,
      "a stale client is told to update");
  Expect(
      dnd::AdviseUpdate(current, older, current) == UpdateAdvice::kUpdateHost,
      "a stale host is named as the one to update");
  Expect(dnd::AdviseUpdate(current, current, newer) ==
             UpdateAdvice::kNewerAvailable,
         "equal versions with a newer release stay compatible");
  Expect(
      dnd::AdviseUpdate(current, current, current) == UpdateAdvice::kUpToDate,
      "equal versions without a newer release are up to date");
  Expect(dnd::AdviseUpdate(current, current, std::nullopt) ==
             UpdateAdvice::kUnknown,
         "no manifest means the player checks manually");
  Expect(dnd::AdviseUpdate(older, current, std::nullopt) ==
             UpdateAdvice::kUpdateMine,
         "the host comparison needs no manifest");
  Expect(dnd::AdviseUpdate(current, newer, newer) == UpdateAdvice::kUpdateMine,
         "the host can be many releases ahead");
  Expect(dnd::AdviseUpdate(newer, current, older) == UpdateAdvice::kUpdateHost,
         "an older manifest does not change who is behind");
  dnd::Version dirty = current;
  dirty.dirty = true;
  Expect(dnd::AdviseUpdate(dirty, current, current) == UpdateAdvice::kUpToDate,
         "dirty does not change the verdict");
}

auto TestStartupRecovery() -> void {
  using dnd::StartupAction;
  Expect(dnd::PlanStartupRecovery(true, false, false) == StartupAction::kNone,
         "a clean folder needs nothing");
  Expect(dnd::PlanStartupRecovery(true, true, false) == StartupAction::kCleanup,
         "a leftover old exe is cleaned up");
  Expect(dnd::PlanStartupRecovery(true, false, true) == StartupAction::kCleanup,
         "a stale download is cleaned up");
  Expect(dnd::PlanStartupRecovery(true, true, true) == StartupAction::kCleanup,
         "old and downloaded files are cleaned up after a swap");
  Expect(dnd::PlanStartupRecovery(false, true, false) ==
             StartupAction::kRestoreOld,
         "a missing exe with an old one is restored");
  Expect(
      dnd::PlanStartupRecovery(false, true, true) == StartupAction::kRestoreOld,
      "the old exe wins over an unfinished download");
  Expect(dnd::PlanStartupRecovery(false, false, true) ==
             StartupAction::kFinishSwap,
         "a missing exe is finished from the downloaded one");
  Expect(dnd::PlanStartupRecovery(false, false, false) == StartupAction::kNone,
         "an empty folder has nothing to recover");

  const std::filesystem::path directory =
      std::filesystem::temp_directory_path() / "dnd_core_tests_update";
  std::error_code cleanup_error;
  std::filesystem::remove_all(directory, cleanup_error);
  std::filesystem::create_directories(directory);
  const auto write = [&directory](std::string_view name,
                                  std::string_view text) -> void {
    std::ofstream file(directory / std::string(name),
                       std::ios::binary | std::ios::trunc);
    file.write(text.data(), static_cast<std::streamsize>(text.size()));
  };
  const auto read = [&directory](std::string_view name) -> std::string {
    std::ifstream file(directory / std::string(name), std::ios::binary);
    return {std::istreambuf_iterator<char>(file),
            std::istreambuf_iterator<char>()};
  };
  const auto exists = [&directory](std::string_view name) -> bool {
    std::error_code code;
    return std::filesystem::exists(directory / std::string(name), code);
  };

  // Подмена завершена: old и недокачанный update убираются, exe не тронут.
  write(dnd::kExecutableName, "new");
  write(dnd::kPreviousExeName, "old");
  write(dnd::kUpdateExeName, "half");
  std::string error;
  Expect(dnd::ApplyStartupRecovery(directory, StartupAction::kCleanup, error),
         "cleanup succeeds");
  Expect(read(dnd::kExecutableName) == "new" &&
             !exists(dnd::kPreviousExeName) && !exists(dnd::kUpdateExeName),
         "cleanup keeps the current exe and removes leftovers");

  // Обрыв после первого rename: старый бинарник возвращается на место.
  write(dnd::kPreviousExeName, "old");
  Expect(
      dnd::ApplyStartupRecovery(directory, StartupAction::kRestoreOld, error),
      "restore old succeeds");
  Expect(read(dnd::kExecutableName) == "old" && !exists(dnd::kPreviousExeName),
         "restore old puts the previous exe back");

  // Обрыв после скачивания: update становится exe.
  write(dnd::kUpdateExeName, "downloaded");
  Expect(
      dnd::ApplyStartupRecovery(directory, StartupAction::kFinishSwap, error),
      "finish swap succeeds");
  Expect(read(dnd::kExecutableName) == "downloaded" &&
             !exists(dnd::kUpdateExeName),
         "finish swap installs the downloaded exe");

  // Недокачанный ассет — тоже остаток обновления: рабочий exe его не читает.
  std::filesystem::create_directories(directory / "assets/fonts");
  write("assets/fonts/DejaVuSans.ttf", "font");
  write("assets/fonts/DejaVuSans.ttf.update", "half");
  Expect(dnd::ApplyStartupRecovery(directory, StartupAction::kCleanup, error),
         "cleanup also handles assets");
  Expect(exists("assets/fonts/DejaVuSans.ttf") &&
             !exists("assets/fonts/DejaVuSans.ttf.update"),
         "a staged asset is removed, the real one stays");

  Expect(dnd::ApplyStartupRecovery(directory, StartupAction::kNone, error),
         "nothing to do is not an error");
  std::filesystem::remove_all(directory, cleanup_error);
}

auto TestCaseInsensitiveOwnership() -> void {
  const dnd::GridMap map(10, 1);
  const dnd::GameSnapshot snapshot{
      .map = map,
      .tokens = {dnd::Token{.id = 1,
                            .name = "hero",
                            .owner = "alice",
                            .kind = dnd::TokenKind::kPlayer,
                            .cell = CellAt(3, 0)}},
      .next_token_id = 2,
  };
  dnd::GameServer server(snapshot, 2, 7);
  Expect(server
             .HandleCommand(dnd::MoveToken{.id = 1, .target = CellAt(4, 0)},
                            "ALICE", false)
             .accepted,
         "the owner controls the character in any case");
  Expect(FogVisibleAt(server.FogState("Alice"), 4, 0),
         "fog is keyed by the folded player name");
  Expect(!server
              .HandleCommand(dnd::MoveToken{.id = 1, .target = CellAt(5, 0)},
                             "bob", false)
              .accepted,
         "another player still cannot move the character");

  Expect(server
             .HandleCommand(dnd::AddToken{.name = "scout",
                                          .cell = CellAt(6, 0),
                                          .kind = dnd::TokenKind::kPlayer,
                                          .max_hp = 10},
                            "", true)
             .accepted,
         "the DM adds a second character");
  Expect(
      !server.HandleCommand(dnd::ClaimToken{.id = 2}, "Alice", false).accepted,
      "a player keeps a single character across case");

  Expect(server
             .HandleCommand(dnd::SetTokenOwner{.id = 2, .owner = "Carol"}, "",
                            true)
             .accepted,
         "the DM assigns an owner by name");
  Expect(
      server.HandleCommand(dnd::ClaimToken{.id = 2}, "carol", false).accepted,
      "the assigned player claims the character in another case");
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
  Expect(server.EventVisibleTo(dm_add.events.front(), "alice"),
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
             !server.EventVisibleTo(hidden_add.events.front(), "alice"),
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
  view.Reset(server.ClientSnapshot("alice"), server.FogState("alice"));
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

auto TestEncounterComposition() -> void {
  // Герой видит двух монстров рядом; третий стоит за пределами обзора и в
  // бой при старте не попадает.
  const dnd::GridMap map(12, 4);
  const dnd::GameSnapshot snapshot{
      .map = map,
      .tokens =
          {
              dnd::Token{.id = 1,
                         .name = "hero",
                         .owner = "alice",
                         .hp = 20,
                         .max_hp = 20,
                         .speed = 9,
                         .attack_bonus = 100,
                         .initiative_bonus = 100,
                         .damage = "1d4",
                         .attack_range = 10,
                         .kind = dnd::TokenKind::kPlayer,
                         .cell = CellAt(0, 0)},
              dnd::Token{.id = 2,
                         .name = "goblin",
                         .owner = "dm",
                         .hp = 20,
                         .max_hp = 20,
                         .ac = 10,
                         .kind = dnd::TokenKind::kMonster,
                         .cell = CellAt(1, 1)},
              dnd::Token{.id = 3,
                         .name = "orc",
                         .owner = "dm",
                         .hp = 20,
                         .max_hp = 20,
                         .ac = 10,
                         .kind = dnd::TokenKind::kMonster,
                         .cell = CellAt(2, 0)},
              dnd::Token{.id = 4,
                         .name = "far",
                         .owner = "dm",
                         .hp = 20,
                         .max_hp = 20,
                         .ac = 10,
                         .conditions = {dnd::Condition{"застигнут врасплох"}},
                         .kind = dnd::TokenKind::kMonster,
                         .cell = CellAt(8, 0)},
          },
      .next_token_id = 5,
  };
  dnd::GameServer server(snapshot, 6, 7);
  const dnd::CommandOutcome start =
      server.HandleCommand(dnd::StartEncounter{}, "", true);
  Expect(start.accepted, "the DM starts a composed encounter");
  const dnd::Encounter before = server.State().EncounterState();
  Expect(before.order.size() == 3 &&
             std::ranges::find(before.order, 4) == before.order.end(),
         "unseen monsters stay out of the encounter");
  Expect(server.State().ActiveTokenId() == 1,
         "the visible party acts first by initiative");

  Expect(server
             .HandleCommand(dnd::MoveToken{.id = 1, .target = CellAt(3, 0)},
                            "alice", false)
             .accepted,
         "the hero moves closer to the hidden monster");
  const dnd::CommandOutcome attack = server.HandleCommand(
      dnd::AttackToken{.attacker = 1, .target = 4}, "alice", false);
  Expect(attack.accepted, "the hero strikes the newly seen monster");
  const dnd::Encounter after = server.State().EncounterState();
  Expect(after.order.size() == 4 && after.order.back() == 4,
         "a struck monster joins at the end of the round");
  Expect(after.turn_index == before.turn_index,
         "joining does not shift the current turn");
  const dnd::Token joined = TokenOf(server.State(), 4);
  Expect(joined.initiative >= 1 && joined.initiative <= 20,
         "the joining monster rolls a real initiative");
  Expect(!HasCondition(joined, "застигнут врасплох"),
         "the surprise condition is spent when the monster joins");
  Expect(std::ranges::any_of(
             attack.events,
             [](const dnd::Event& event) -> bool {
               if (!std::holds_alternative<dnd::LogLineAdded>(event)) {
                 return false;
               }
               return std::get<dnd::LogLineAdded>(event).line.text.find(
                          "вступает в бой") != std::string::npos;
             }),
         "the log announces the joining monster");

  // Игрок состав не правит.
  Expect(!server
              .HandleCommand(
                  dnd::SetEncounterComposition{.id = 4, .joined = false},
                  "alice", false)
              .accepted,
         "players cannot edit the encounter composition");

  // DM выводит монстра из боя.
  Expect(
      server
          .HandleCommand(dnd::SetEncounterComposition{.id = 4, .joined = false},
                         "", true)
          .accepted,
      "the DM removes a monster from the encounter");
  Expect(server.State().EncounterState().order.size() == 3,
         "the removed monster leaves the order");

  // Новый токен в бою получает настоящий бросок, а не нулевую инициативу.
  const dnd::CommandOutcome added =
      server.HandleCommand(dnd::AddToken{.name = "troll",
                                         .cell = CellAt(10, 3),
                                         .owner = "dm",
                                         .kind = dnd::TokenKind::kMonster,
                                         .max_hp = 10},
                           "", true);
  Expect(added.accepted, "the DM adds a monster during the encounter");
  const dnd::Encounter with_troll = server.State().EncounterState();
  Expect(with_troll.order.size() == 4 && with_troll.order.back() == 5,
         "the added monster joins the encounter");
  const dnd::Token troll = TokenOf(server.State(), 5);
  Expect(troll.initiative >= 1 && troll.initiative <= 20,
         "the added monster gets a real initiative roll");
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
    Expect(server.EventVisibleTo(start.events.front(), "alice"),
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

  const dnd::GameSnapshot before_move = server.Snapshot();
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
  view.Reset(before_move, server.FullFogState());
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

auto TestTurnEconomy() -> void {
  const dnd::GridMap map(16, 3);
  const dnd::GameSnapshot snapshot{
      .map = map,
      .tokens =
          {
              dnd::Token{.id = 1,
                         .name = "hero",
                         .owner = "alice",
                         .initiative = 20,
                         .ac = 10,
                         .speed = 9,
                         .attack_bonus = 100,
                         .initiative_bonus = 100,
                         .damage = "1d4",
                         .kind = dnd::TokenKind::kPlayer,
                         .cell = CellAt(1, 1)},
              dnd::Token{.id = 2,
                         .name = "rival",
                         .owner = "bob",
                         .initiative = 10,
                         .ac = 1,
                         .speed = 9,
                         .attack_bonus = 100,
                         .damage = "1d4",
                         .kind = dnd::TokenKind::kPlayer,
                         .cell = CellAt(2, 1)},
          },
      .next_token_id = 3,
  };
  dnd::GameServer server(snapshot, 12, 7);
  Expect(server.HandleCommand(dnd::StartEncounter{}, "", true).accepted,
         "the DM starts a turn-economy encounter");
  Expect(server.State().ActiveTokenId() == 1, "the hero acts first");

  const dnd::CommandOutcome rival_move = server.HandleCommand(
      dnd::MoveToken{.id = 2, .target = CellAt(2, 2)}, "bob", false);
  Expect(
      !rival_move.accepted && rival_move.reject_reason == "сейчас не ваш ход",
      "a player cannot move on someone else's turn");
  const dnd::CommandOutcome rival_attack = server.HandleCommand(
      dnd::AttackToken{.attacker = 2, .target = 1}, "bob", false);
  Expect(!rival_attack.accepted &&
             rival_attack.reject_reason == "сейчас не ваш ход",
         "an attack outside your turn needs a reaction");
  Expect(server
             .HandleCommand(
                 dnd::AttackToken{.attacker = 2, .target = 1, .reaction = true},
                 "bob", false)
             .accepted,
         "an adjacent target can be attacked as a reaction");
  Expect(TokenOf(server.State(), 2).reaction_used, "the reaction is spent");
  const dnd::CommandOutcome second_reaction = server.HandleCommand(
      dnd::AttackToken{.attacker = 2, .target = 1, .reaction = true}, "bob",
      false);
  Expect(!second_reaction.accepted &&
             second_reaction.reject_reason == "реакция уже потрачена",
         "only one reaction per round");

  Expect(
      server
          .HandleCommand(
              dnd::UseTurnAction{.id = 1, .kind = dnd::TurnActionKind::kDodge},
              "alice", false)
          .accepted,
      "the active player spends the action on Dodge");
  const dnd::Token dodging = TokenOf(server.State(), 1);
  Expect(dodging.action_used && dodging.dodging,
         "Dodge marks the action spent and the token dodging");
  const dnd::CommandOutcome spent_action = server.HandleCommand(
      dnd::AttackToken{.attacker = 1, .target = 2}, "alice", false);
  Expect(!spent_action.accepted &&
             spent_action.reject_reason == "действие уже потрачено",
         "one action per turn");
  Expect(server
             .HandleCommand(dnd::MoveToken{.id = 1, .target = CellAt(3, 1)},
                            "alice", false)
             .accepted,
         "movement is separate from the action");

  Expect(server.HandleCommand(dnd::NextTurn{}, "", true).accepted,
         "the DM ends the hero turn");
  Expect(server.State().ActiveTokenId() == 2, "the rival acts next");
  Expect(server
             .HandleCommand(
                 dnd::AttackToken{.attacker = 1, .target = 2, .reaction = true},
                 "alice", false)
             .accepted,
         "the hero reacts on the rival's turn");
  Expect(TokenOf(server.State(), 1).reaction_used,
         "the hero reaction is spent");
  Expect(server
             .HandleCommand(dnd::AttackToken{.attacker = 2, .target = 1}, "bob",
                            false)
             .accepted,
         "the rival uses the action on their own turn");

  Expect(server.HandleCommand(dnd::NextTurn{}, "", true).accepted,
         "the round returns to the hero");
  const dnd::Token fresh = TokenOf(server.State(), 1);
  Expect(!fresh.action_used && !fresh.reaction_used && !fresh.dodging &&
             fresh.movement_bonus == 0,
         "a new turn restores action, reaction and Dodge");
  Expect(
      server
          .HandleCommand(
              dnd::UseTurnAction{.id = 1, .kind = dnd::TurnActionKind::kDash},
              "alice", false)
          .accepted,
      "Dash spends the action");
  const dnd::Token dashed = TokenOf(server.State(), 1);
  Expect(dashed.movement_bonus == dnd::SpeedToCells(dashed.speed),
         "Dash adds a full speed of movement");
  Expect(server
             .HandleCommand(dnd::MoveToken{.id = 1, .target = CellAt(10, 1)},
                            "alice", false)
             .accepted,
         "the dash budget covers a longer move");
  Expect(TokenOf(server.State(), 1).movement_used == 7,
         "the longer move is charged normally");

  Expect(server
             .HandleCommand(
                 dnd::UpdateTokenCombat{
                     .id = 1, .conditions = {dnd::Condition{"оглушён"}}},
                 "", true)
             .accepted,
         "the DM stuns the hero");
  const dnd::CommandOutcome stunned_move = server.HandleCommand(
      dnd::MoveToken{.id = 1, .target = CellAt(11, 1)}, "alice", false);
  Expect(!stunned_move.accepted &&
             stunned_move.reject_reason == "персонаж недееспособен",
         "an incapacitated token does not move");
  const dnd::CommandOutcome stunned_attack = server.HandleCommand(
      dnd::AttackToken{.attacker = 1, .target = 2}, "alice", false);
  Expect(!stunned_attack.accepted &&
             stunned_attack.reject_reason == "персонаж недееспособен",
         "an incapacitated token does not attack");
  const dnd::CommandOutcome stunned_action = server.HandleCommand(
      dnd::UseTurnAction{.id = 1, .kind = dnd::TurnActionKind::kHide}, "alice",
      false);
  Expect(!stunned_action.accepted &&
             stunned_action.reject_reason == "персонаж недееспособен",
         "an incapacitated token has no actions");
}

auto TestMovementPathsAndDiagonals() -> void {
  const dnd::GridMap corridor(6, 1);
  const dnd::GameSnapshot snapshot{
      .map = corridor,
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
                         .cell = CellAt(3, 0)},
              dnd::Token{.id = 3,
                         .name = "ally",
                         .owner = "bob",
                         .kind = dnd::TokenKind::kPlayer,
                         .cell = CellAt(1, 0)},
          },
      .next_token_id = 4,
  };
  dnd::GameServer server(snapshot, 6, 5);

  Expect(!server
              .HandleCommand(dnd::MoveToken{.id = 1, .target = CellAt(1, 0)},
                             "alice", false)
              .accepted,
         "a move cannot end on an allied character");
  const dnd::CommandOutcome through_monster = server.HandleCommand(
      dnd::MoveToken{.id = 1, .target = CellAt(5, 0)}, "alice", false);
  Expect(!through_monster.accepted, "a monster blocks the path outside combat");
  const dnd::CommandOutcome onto_monster = server.HandleCommand(
      dnd::MoveToken{.id = 1, .target = CellAt(3, 0)}, "alice", false);
  Expect(
      !onto_monster.accepted && onto_monster.reject_reason == "клетка занята",
      "players cannot end a move on an occupied cell");
  Expect(server
             .HandleCommand(dnd::MoveToken{.id = 1, .target = CellAt(2, 0)},
                            "alice", false)
             .accepted,
         "allied characters do not block the path");
  Expect(TokenOf(server.State(), 1).movement_used == 0,
         "outside combat movement is not charged");

  // Правило 5–10–5: диагонали стоят 1, 2, 1, 2 клетки.
  const dnd::GridMap open(6, 6);
  const dnd::GameSnapshot diagonal_snapshot{
      .map = open,
      .tokens = {dnd::Token{.id = 1,
                            .name = "hero",
                            .owner = "alice",
                            .speed = 9,
                            .kind = dnd::TokenKind::kPlayer,
                            .cell = CellAt(0, 0)}},
      .next_token_id = 2,
  };
  dnd::GameServer diagonals(diagonal_snapshot, 6, 7);
  Expect(diagonals.HandleCommand(dnd::StartEncounter{}, "", true).accepted,
         "encounter starts for the diagonal test");
  const auto step = [&diagonals](dnd::Cell cell) -> bool {
    return diagonals
        .HandleCommand(dnd::MoveToken{.id = 1, .target = cell}, "alice", false)
        .accepted;
  };
  Expect(step(CellAt(1, 1)), "the first diagonal costs 5 feet");
  Expect(TokenOf(diagonals.State(), 1).movement_used == 1 &&
             TokenOf(diagonals.State(), 1).diagonal_parity == 1,
         "the first diagonal spends one cell and flips parity");
  Expect(step(CellAt(2, 2)), "the second diagonal costs 10 feet");
  Expect(TokenOf(diagonals.State(), 1).movement_used == 3 &&
             TokenOf(diagonals.State(), 1).diagonal_parity == 0,
         "the second diagonal spends two cells");
  Expect(step(CellAt(3, 3)) && step(CellAt(4, 4)),
         "the third and fourth diagonals fit the remaining budget");
  Expect(TokenOf(diagonals.State(), 1).movement_used == 6,
         "the full speed is spent after four diagonals");
  Expect(!step(CellAt(4, 5)), "a spent budget blocks the next step");
  Expect(diagonals.HandleCommand(dnd::NextTurn{}, "", true).accepted,
         "the turn passes to the next token");
  Expect(diagonals.HandleCommand(dnd::EndEncounter{}, "", true).accepted,
         "the encounter ends");
  Expect(TokenOf(diagonals.State(), 1).diagonal_parity == 0,
         "ending combat resets the diagonal parity");
}

auto TestCrawlJumpFall() -> void {
  // Ползание: не встаём, платим двойную цену, остаёмся «сбит с ног».
  const dnd::GridMap open(8, 8);
  const dnd::GameSnapshot prone_snapshot{
      .map = open,
      .tokens = {dnd::Token{.id = 1,
                            .name = "hero",
                            .owner = "alice",
                            .speed = 9,
                            .conditions = {dnd::Condition{"сбит с ног"}},
                            .kind = dnd::TokenKind::kPlayer,
                            .cell = CellAt(1, 1)}},
      .next_token_id = 2,
  };
  dnd::GameServer prone_server(prone_snapshot, 6, 11);
  Expect(prone_server.HandleCommand(dnd::StartEncounter{}, "", true).accepted,
         "the crawl encounter starts");
  const dnd::CommandOutcome crawled = prone_server.HandleCommand(
      dnd::MoveToken{.id = 1, .target = CellAt(2, 1), .crawl = true}, "alice",
      false);
  Expect(crawled.accepted, "a prone character can crawl");
  const dnd::Token after_crawl = TokenOf(prone_server.State(), 1);
  Expect(after_crawl.cell == CellAt(2, 1), "crawling moves the token");
  Expect(HasCondition(after_crawl, "сбит с ног"),
         "crawling keeps the prone condition");
  Expect(after_crawl.movement_used == 2, "crawling costs two cells per cell");
  Expect(dnd::StandUpCostCells(after_crawl) == 3 &&
             dnd::RemainingMovementCells(after_crawl) == 1,
         "standing up costs half the speed and leaves the same reach as M");
  const dnd::CommandOutcome stands = prone_server.HandleCommand(
      dnd::MoveToken{.id = 1, .target = CellAt(2, 2)}, "alice", false);
  Expect(stands.accepted, "a normal move stands up first");
  const dnd::Token after_stand = TokenOf(prone_server.State(), 1);
  Expect(!HasCondition(after_stand, "сбит с ног"),
         "standing removes the prone condition");
  Expect(after_stand.movement_used == 6,
         "standing and the step spend half speed plus the path");

  // Прыжок: через воду можно, через стену и дальше 10 футов — нет.
  dnd::GridMap water_map(8, 8);
  water_map.SetTerrain(CellAt(2, 1), dnd::Terrain::kWater);
  const dnd::GameSnapshot jump_snapshot{
      .map = water_map,
      .tokens = {dnd::Token{.id = 1,
                            .name = "jumper",
                            .owner = "alice",
                            .speed = 3,
                            .kind = dnd::TokenKind::kPlayer,
                            .cell = CellAt(1, 1)}},
      .next_token_id = 2,
  };
  dnd::GameServer jump_server(jump_snapshot, 6, 12);
  Expect(jump_server.HandleCommand(dnd::StartEncounter{}, "", true).accepted,
         "the jump encounter starts");
  Expect(!jump_server
              .HandleCommand(dnd::MoveToken{.id = 1, .target = CellAt(3, 1)},
                             "alice", false)
              .accepted,
         "a walk cannot cross water");
  Expect(jump_server
             .HandleCommand(
                 dnd::MoveToken{.id = 1, .target = CellAt(3, 1), .jump = true},
                 "alice", false)
             .accepted,
         "a jump crosses water");
  const dnd::Token jumped = TokenOf(jump_server.State(), 1);
  Expect(jumped.cell == CellAt(3, 1) && jumped.movement_used == 2,
         "a two-cell jump lands and spends two cells");
  Expect(!jump_server
              .HandleCommand(
                  dnd::MoveToken{.id = 1, .target = CellAt(6, 1), .jump = true},
                  "alice", false)
              .accepted,
         "a jump farther than 10 feet is rejected");

  dnd::GridMap wall_map(8, 8);
  wall_map.SetTerrain(CellAt(2, 2), dnd::Terrain::kWall);
  const dnd::GameSnapshot wall_snapshot{
      .map = wall_map,
      .tokens = {dnd::Token{.id = 1,
                            .name = "jumper",
                            .owner = "alice",
                            .speed = 9,
                            .kind = dnd::TokenKind::kPlayer,
                            .cell = CellAt(1, 2)}},
      .next_token_id = 2,
  };
  dnd::GameServer wall_server(wall_snapshot, 6, 13);
  Expect(!wall_server
              .HandleCommand(
                  dnd::MoveToken{.id = 1, .target = CellAt(3, 2), .jump = true},
                  "alice", false)
              .accepted,
         "a wall cannot be jumped over");

  // Падение: 1d6 за каждые 10 футов, «сбит с ног» только при уроне.
  Expect(dnd::FallDamageDice(9) == 0 && dnd::FallDamageDice(10) == 1 &&
             dnd::FallDamageDice(200) == 20 && dnd::FallDamageDice(2000) == 20,
         "falling damage caps at 20d6");
  const dnd::GameSnapshot fall_snapshot{
      .map = open,
      .tokens = {dnd::Token{.id = 1,
                            .name = "faller",
                            .owner = "alice",
                            .hp = 20,
                            .max_hp = 20,
                            .kind = dnd::TokenKind::kPlayer,
                            .cell = CellAt(1, 1)}},
      .next_token_id = 2,
  };
  dnd::GameServer fall_server(fall_snapshot, 6, 14);
  Expect(fall_server.HandleCommand(dnd::FallToken{.id = 1, .feet = 5}, "", true)
             .accepted,
         "a short fall is accepted");
  Expect(TokenOf(fall_server.State(), 1).hp == 20 &&
             !HasCondition(TokenOf(fall_server.State(), 1), "сбит с ног"),
         "a fall under 10 feet deals no damage and does not knock prone");
  Expect(
      fall_server.HandleCommand(dnd::FallToken{.id = 1, .feet = 30}, "", true)
          .accepted,
      "a 30-foot fall is accepted");
  const dnd::Token fallen = TokenOf(fall_server.State(), 1);
  Expect(fallen.hp < 20, "falling deals bludgeoning damage");
  Expect(HasCondition(fallen, "сбит с ног"),
         "taking fall damage knocks the character prone");

  // Иммунитет к дробящему урону: урона нет, «сбит с ног» тоже.
  dnd::GameSnapshot immune_snapshot = fall_snapshot;
  immune_snapshot.tokens[0].immune =
      dnd::DamageTypeBit(dnd::DamageType::kBludgeoning);
  dnd::GameServer immune_server(immune_snapshot, 6, 15);
  Expect(immune_server
             .HandleCommand(dnd::FallToken{.id = 1, .feet = 100}, "", true)
             .accepted,
         "an immune character still falls");
  const dnd::Token immune = TokenOf(immune_server.State(), 1);
  Expect(immune.hp == 20 && !HasCondition(immune, "сбит с ног"),
         "immunity to bludgeoning prevents both damage and prone");
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
    Expect(server.EventVisibleTo(placed.events.front(), "alice"),
           "templates are visible to players");
  }
  Expect(server.State().Aoe() == set.aoe, "server keeps the template");
  Expect(server.ClientSnapshot("alice").aoe == set.aoe,
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
  map.SetTerrain(CellAt(4, 1), dnd::Terrain::kLava);
  map.SetTerrain(CellAt(5, 2), dnd::Terrain::kTree);
  dnd::CellProps props = map.PropsAt(CellAt(0, 3));
  props.flags = dnd::kCellRoad | dnd::kCellClimbable;
  props.water = dnd::WaterDepth::kShallow;
  props.current_dir = 2;
  props.current_strength = 3;
  props.slippery_dc = 15;
  props.hazard = 2;
  props.light = static_cast<std::uint8_t>(dnd::LightLevel::kDim);
  props.obscurement = dnd::Obscurement::kHeavy;
  dnd::SetCoverOnSide(props.cover_sides, 1, dnd::CoverLevel::kHalf);
  dnd::SetCoverOnSide(props.cover_sides, 2, dnd::CoverLevel::kThreeQuarters);
  map.SetProps(CellAt(0, 3), props);
  map.SetAmbientLight(dnd::LightLevel::kDim);
  const std::vector<std::uint8_t> party_fog(24, 0);
  std::vector<std::uint8_t> alice_fog(24, 0);
  alice_fog[13] = 1;
  const dnd::GameSnapshot original{
      .map = map,
      .tokens =
          {
              dnd::Token{
                  .id = 4,
                  .name = "hero",
                  .owner = "alice",
                  .hp = 7,
                  .max_hp = 12,
                  .temp_hp = 3,
                  .hit_die = 10,
                  .con_modifier = 2,
                  .hit_dice_spent = 2,
                  .initiative = 15,
                  .ac = 16,
                  .speed = 9,
                  .swim_speed = 4,
                  .climb_speed = 2,
                  .darkvision_cells = 4,
                  .light_bright_cells = 2,
                  .light_dim_cells = 4,
                  .notes = "Атака: +5, 1d8+3",
                  .attack_bonus = 5,
                  .save_bonus = 2,
                  .damage = "1d8+3 огонь",
                  .resist = dnd::DamageTypeBit(dnd::DamageType::kFire),
                  .vulnerable = dnd::DamageTypeBit(dnd::DamageType::kCold),
                  .immune = dnd::DamageTypeBit(dnd::DamageType::kPoison),
                  .attack_range = 3,
                  .conditions = {dnd::Condition{"сбит с ног"},
                                 dnd::Condition{"концентрация", 60, 12}},
                  .xp = 900,
                  .movement_used = 4,
                  .diagonal_parity = 1,
                  .kind = dnd::TokenKind::kPlayer,
                  .color = 0x507AC8U,
                  .portrait = "hero.png",
                  .cell = CellAt(1, 1),
                  .hazards_done = {1, 5}},
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
      .background =
          dnd::MapBackground{
              .image = "dungeon.png",
              .opacity = 60,
              .scale = 140,
          },
      .revealed = {CellAt(0, 0), CellAt(5, 3)},
      .fog = {dnd::FogRecord{.owner = {}, .explored = party_fog},
              dnd::FogRecord{.owner = "alice", .explored = alice_fog}},
      .game_time_seconds = 12345,
      .last_long_rest_seconds = 12000,
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
    Expect(!loaded->map.IsBlocked(CellAt(4, 1)) &&
               loaded->map.TerrainAt(CellAt(4, 1)) == dnd::Terrain::kLava,
           "lava survives the roundtrip");
    Expect(loaded->map.HazardAt(CellAt(4, 1)) != nullptr,
           "lava keeps its default damage profile");
    Expect(loaded->map.IsDifficultTerrain(CellAt(5, 2)),
           "trees survive the roundtrip");
    const dnd::CellProps& loaded_props = loaded->map.PropsAt(CellAt(0, 3));
    Expect(loaded_props.flags == (dnd::kCellRoad | dnd::kCellClimbable) &&
               loaded_props.water == dnd::WaterDepth::kShallow &&
               loaded_props.current_dir == 2 &&
               loaded_props.current_strength == 3 &&
               loaded_props.slippery_dc == 15 && loaded_props.hazard == 2 &&
               dnd::CoverOnSide(loaded_props.cover_sides, 1) ==
                   dnd::CoverLevel::kHalf &&
               dnd::CoverOnSide(loaded_props.cover_sides, 2) ==
                   dnd::CoverLevel::kThreeQuarters,
           "cell properties survive the roundtrip");
    Expect(loaded_props.light ==
                   static_cast<std::uint8_t>(dnd::LightLevel::kDim) &&
               loaded_props.obscurement == dnd::Obscurement::kHeavy,
           "cell light and obscurement survive the roundtrip");
    Expect(loaded->map.AmbientLight() == dnd::LightLevel::kDim,
           "the map ambient light survives the roundtrip");
    Expect(loaded->tokens.size() == 2, "token count survives the roundtrip");
    Expect(loaded->tokens.front().name == "hero", "token name survives");
    Expect(loaded->revealed.size() == 2 &&
               loaded->revealed.front() == CellAt(0, 0) &&
               loaded->revealed.back() == CellAt(5, 3),
           "revealed cells survive the roundtrip");
    Expect(loaded->fog.size() == 2 && loaded->fog.front().owner.empty(),
           "the party fog survives the roundtrip");
    Expect(loaded->fog.size() == 2 && loaded->fog.back().owner == "alice",
           "the player fog survives the roundtrip");
    Expect(loaded->fog.size() == 2 &&
               loaded->fog.back().explored.size() == 24 &&
               loaded->fog.back().explored[13] == 1,
           "explored cells survive the roundtrip");
    Expect(loaded->game_time_seconds == 12345 &&
               loaded->last_long_rest_seconds == 12000,
           "the game clock and long rest time survive the roundtrip");
    Expect(loaded->tokens.front().owner == "alice", "token owner survives");
    Expect(loaded->tokens.front().hp == 7, "token hp survives");
    Expect(loaded->tokens.front().xp == 900, "token xp survives");
    Expect(loaded->tokens.front().hit_die == 10 &&
               loaded->tokens.front().con_modifier == 2,
           "hit die and Constitution survive the roundtrip");
    Expect(loaded->tokens.front().hit_dice_spent == 2,
           "spent hit dice survive the roundtrip");
    Expect(loaded->tokens.front().diagonal_parity == 1,
           "diagonal parity survives the roundtrip");
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
    Expect(loaded->tokens.front().damage == "1d8+3 огонь",
           "token damage expression survives");
    Expect(loaded->tokens.front().resist ==
                   dnd::DamageTypeBit(dnd::DamageType::kFire) &&
               loaded->tokens.front().vulnerable ==
                   dnd::DamageTypeBit(dnd::DamageType::kCold) &&
               loaded->tokens.front().immune ==
                   dnd::DamageTypeBit(dnd::DamageType::kPoison),
           "damage defences survive the roundtrip");
    Expect(loaded->tokens.front().attack_range == 3,
           "attack range survives the roundtrip");
    Expect(loaded->tokens.front().swim_speed == 4 &&
               loaded->tokens.front().climb_speed == 2,
           "swim and climb speeds survive the roundtrip");
    Expect(loaded->tokens.front().darkvision_cells == 4 &&
               loaded->tokens.front().light_bright_cells == 2 &&
               loaded->tokens.front().light_dim_cells == 4,
           "darkvision and token light survive the roundtrip");
    Expect(
        loaded->tokens.front().hazards_done == std::vector<std::uint8_t>{1, 5},
        "hazards fired this turn survive the roundtrip");
    Expect(loaded->tokens.front().conditions.size() == 2 &&
               loaded->tokens.front().conditions.back().seconds == 60 &&
               loaded->tokens.front().conditions.back().save_dc == 12,
           "condition duration and КС survive the roundtrip");
    Expect(loaded->tokens.front().conditions.size() == 2,
           "token conditions survive");
    Expect(loaded->tokens.front().movement_used == 4,
           "token spent movement survives");
    Expect(loaded->tokens.front().kind == dnd::TokenKind::kPlayer,
           "token kind survives");
    Expect(loaded->tokens.front().color == 0x507AC8U, "token colour survives");
    Expect(loaded->tokens.front().portrait == "hero.png",
           "token portrait survives");
    Expect(loaded->tokens.front().cell == CellAt(1, 1), "token cell survives");
    Expect(loaded->next_token_id == 7, "next token id survives");
    Expect(loaded->encounter.active && loaded->encounter.round == 3,
           "encounter survives the roundtrip");
    Expect(loaded->encounter.order == std::vector<dnd::TokenId>{4, 6} &&
               loaded->encounter.turn_index == 1,
           "turn order survives the roundtrip");
    Expect(loaded->aoe == original.aoe, "aoe template survives the roundtrip");
    Expect(loaded->background.image == "dungeon.png" &&
               loaded->background.opacity == 60 &&
               loaded->background.scale == 140,
           "map background survives the roundtrip");
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

// Заголовок файла версии: magic, версия, размер, payload. Пишем вручную,
// чтобы проверить чтение файлов прошлых релизов, а не только roundtrip.
auto WriteVersionedFile(const std::filesystem::path& path, std::uint32_t magic,
                        std::uint32_t version,
                        const std::vector<std::uint8_t>& payload) -> void {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  const auto write32 = [&out](std::uint32_t value) -> void {
    for (int shift = 0; shift < 32; shift += 8) {
      out.put(static_cast<char>((value >> shift) & 0xFFU));
    }
  };
  write32(magic);
  write32(version);
  write32(static_cast<std::uint32_t>(payload.size()));
  out.write(reinterpret_cast<const char*>(payload.data()),
            static_cast<std::streamsize>(payload.size()));
}

// Обещание раздела 7: файлы прошлых релизов открываются. Версия задаёт набор
// полей снимка, поэтому проверяем всю поддерживаемую линейку.
auto TestReadsPreviousVersions() -> void {
  dnd::GridMap old_map(5, 4);
  old_map.SetAmbientLight(dnd::LightLevel::kDim);
  dnd::CellProps old_props = old_map.PropsAt(CellAt(0, 0));
  old_props.light = static_cast<std::uint8_t>(dnd::LightLevel::kDark);
  old_props.obscurement = dnd::Obscurement::kHeavy;
  old_map.SetProps(CellAt(0, 0), old_props);
  const dnd::GameSnapshot original{
      .map = old_map,
      .tokens =
          {
              dnd::Token{
                  .id = 1,
                  .name = "hero",
                  .hp = 5,
                  .max_hp = 10,
                  .hit_dice_spent = 1,
                  .darkvision_cells = 4,
                  .light_bright_cells = 2,
                  .light_dim_cells = 4,
                  .resist = dnd::DamageTypeBit(dnd::DamageType::kFire),
                  .attack_range = 3,
                  .conditions = {dnd::Condition{"оглушён", 60, 12}},
                  .kind = dnd::TokenKind::kPlayer,
                  .cell = CellAt(1, 1),
                  .action_used = true,
                  .dodging = true,
              },
          },
      .next_token_id = 2,
      .game_time_seconds = 7200,
      .last_long_rest_seconds = 3600,
  };
  constexpr std::uint32_t save_magic = 0x53444E44U;
  constexpr std::uint32_t campaign_magic = 0x43444E44U;
  const std::filesystem::path save_path =
      std::filesystem::temp_directory_path() / "dnd_core_tests_old_save.bin";

  for (std::uint32_t version = dnd::kMinSaveVersion;
       version <= dnd::kSaveVersion; ++version) {
    WriteVersionedFile(save_path, save_magic, version,
                       dnd::EncodeSnapshotAs(original, version));
    const std::optional<dnd::GameSnapshot> loaded =
        dnd::ReadSnapshot(save_path);
    Expect(loaded.has_value(), "a previous save version reads back");
    if (!loaded.has_value()) {
      continue;
    }
    const dnd::Token& token = loaded->tokens.front();
    Expect(token.conditions.size() == 1 &&
               token.conditions.front().seconds == 60 &&
               token.conditions.front().save_dc == 12,
           "condition duration and КС survive previous versions");
    Expect(token.hit_dice_spent == 1,
           "spent hit dice survive previous versions");
    Expect(token.action_used == (version >= 28) &&
               token.dodging == (version >= 28),
           "turn economy fields appear with their version");
    Expect(token.resist == (version >= 29
                                ? dnd::DamageTypeBit(dnd::DamageType::kFire)
                                : 0U) &&
               token.attack_range == (version >= 29 ? 3 : 0),
           "damage defences appear with their version");
    Expect(loaded->game_time_seconds == (version >= 30 ? 7200 : 0) &&
               loaded->last_long_rest_seconds == (version >= 30 ? 3600 : -1),
           "the game clock appears with its version");
    Expect(token.darkvision_cells == (version >= 32 ? 4 : 0) &&
               token.light_bright_cells == (version >= 32 ? 2 : 0) &&
               token.light_dim_cells == (version >= 32 ? 4 : 0),
           "darkvision and token light appear with their version");
    Expect(
        loaded->map.AmbientLight() ==
            (version >= 32 ? dnd::LightLevel::kDim : dnd::LightLevel::kBright),
        "the ambient light appears with its version");
    Expect(
        loaded->map.PropsAt(CellAt(0, 0)).light ==
            (version >= 32 ? static_cast<std::uint8_t>(dnd::LightLevel::kDark)
                           : dnd::kAmbientLight),
        "cell light appears with its version");
  }

  WriteVersionedFile(save_path, save_magic, dnd::kMinSaveVersion - 1,
                     dnd::EncodeSnapshotAs(original, dnd::kMinSaveVersion));
  Expect(!dnd::ReadSnapshot(save_path).has_value(),
         "a save older than the supported window is rejected");
  WriteVersionedFile(save_path, save_magic, dnd::kSaveVersion + 1,
                     dnd::EncodeSnapshotAs(original, dnd::kSaveVersion));
  Expect(!dnd::ReadSnapshot(save_path).has_value(),
         "a save from a newer version is rejected");
  std::filesystem::remove(save_path);

  const dnd::Campaign campaign = dnd::MakeCampaign(original, "Пролог");
  const std::filesystem::path campaign_path =
      std::filesystem::temp_directory_path() /
      "dnd_core_tests_old_campaign.dndcamp";
  for (std::uint32_t version = dnd::kMinCampaignVersion;
       version <= dnd::kCampaignVersion; ++version) {
    WriteVersionedFile(campaign_path, campaign_magic, version,
                       dnd::EncodeCampaignAs(campaign, version));
    const std::optional<dnd::Campaign> loaded =
        dnd::ReadCampaign(campaign_path);
    Expect(loaded.has_value(), "a previous campaign version reads back");
    if (!loaded.has_value()) {
      continue;
    }
    Expect(
        loaded->scenes.size() == 1 && loaded->scenes.front().name == "Пролог",
        "scene names survive previous campaign versions");
    Expect(loaded->scenes.front().id == 1 && loaded->next_scene_id == 2,
           "campaign scene ids are derived for old files and kept for new");
    const dnd::Token& token = loaded->scenes.front().snapshot.tokens.front();
    Expect(
        token.conditions.size() == 1 && token.conditions.front().save_dc == 12,
        "condition КС survives previous campaign versions");
    Expect(token.action_used == (version >= 15),
           "campaign turn economy appears with its version");
    Expect(
        token.resist ==
            (version >= 15 ? dnd::DamageTypeBit(dnd::DamageType::kFire) : 0U),
        "campaign damage defences appear with their version");
    Expect(loaded->scenes.front().snapshot.game_time_seconds ==
               (version >= 16 ? 7200 : 0),
           "the campaign game clock appears with its version");
    Expect(token.darkvision_cells == (version >= 19 ? 4 : 0),
           "campaign darkvision appears with its version");
  }

  WriteVersionedFile(campaign_path, campaign_magic,
                     dnd::kMinCampaignVersion - 1,
                     dnd::EncodeCampaignAs(campaign, dnd::kMinCampaignVersion));
  Expect(!dnd::ReadCampaign(campaign_path).has_value(),
         "a campaign older than the supported window is rejected");
  WriteVersionedFile(campaign_path, campaign_magic, dnd::kCampaignVersion + 1,
                     dnd::EncodeCampaignAs(campaign, dnd::kCampaignVersion));
  Expect(!dnd::ReadCampaign(campaign_path).has_value(),
         "a campaign from a newer version is rejected");
  std::filesystem::remove(campaign_path);
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

  campaign.scenes.push_back(dnd::Scene{
      .id = dnd::NextSceneId(campaign), .name = "Сцена 2", .snapshot = second});
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
  campaign.scenes.front().note = "Герои спустились в подземелье";

  const std::filesystem::path path =
      std::filesystem::temp_directory_path() / "dnd_core_tests_campaign.bin";
  Expect(dnd::WriteCampaign(campaign, path), "campaign writes");
  const std::optional<dnd::Campaign> loaded = dnd::ReadCampaign(path);
  Expect(loaded.has_value(), "campaign reads back");
  if (loaded.has_value()) {
    Expect(loaded->scenes.size() == 2, "scene count survives the roundtrip");
    Expect(loaded->active == 1, "active scene survives the roundtrip");
    Expect(loaded->scenes.front().id == 1 && loaded->scenes.back().id == 2 &&
               loaded->next_scene_id == 3,
           "scene ids and the id counter survive the roundtrip");
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

  // Библиотека кампаний: только *.dndcamp, по алфавиту.
  const std::filesystem::path library =
      std::filesystem::temp_directory_path() / "dnd_core_tests_library";
  std::error_code cleanup_error;
  std::filesystem::remove_all(library, cleanup_error);
  std::filesystem::create_directories(library);
  Expect(dnd::WriteCampaign(campaign, library / "b_campaign.dndcamp"),
         "second campaign writes");
  Expect(dnd::WriteCampaign(campaign, library / "a_campaign.dndcamp"),
         "first campaign writes");
  {
    std::ofstream out(library / "notes.txt", std::ios::binary);
    out.write("not a campaign", 14);
  }
  const std::vector<std::string> files = dnd::ListCampaignFiles(library);
  Expect(files.size() == 2 && files.front() == "a_campaign.dndcamp" &&
             files.back() == "b_campaign.dndcamp",
         "campaign library lists only .dndcamp files sorted");
  Expect(dnd::ListCampaignFiles(library / "nowhere").empty(),
         "a missing campaign directory lists nothing");
  std::filesystem::remove_all(library, cleanup_error);
}

// Кампания уходит на диск в фоновом потоке: кадр не ждёт, а новая просьба
// заменяет ещё не начатую запись, поэтому на диске остаётся свежий мир.
auto TestCampaignSaver() -> void {
  const dnd::GameSnapshot map_snapshot{
      .map = dnd::GridMap(4, 4),
      .next_token_id = 1,
  };
  dnd::Campaign campaign = dnd::MakeCampaign(map_snapshot, "Фон");
  campaign.scenes.push_back(dnd::Scene{.id = dnd::NextSceneId(campaign),
                                       .name = "Вторая",
                                       .snapshot = map_snapshot});
  campaign.active = 1;

  const std::filesystem::path path =
      std::filesystem::temp_directory_path() / "dnd_core_tests_saver.dndcamp";
  std::error_code cleanup_error;
  std::filesystem::remove(path, cleanup_error);

  dnd::CampaignSaver saver;
  Expect(!saver.Busy(), "a saver starts idle");
  saver.Request(campaign, path);
  Expect(saver.Busy(), "a request keeps the saver busy until the disk answers");
  Expect(!saver.TakeResult().has_value(),
         "nothing is reported before the write finishes");

  int waited = 0;
  while (saver.Busy() && waited < 2000) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    ++waited;
  }
  const std::optional<bool> result = saver.TakeResult();
  Expect(result.has_value() && *result, "background save reports success");
  Expect(!saver.TakeResult().has_value(), "a result is reported only once");
  const std::optional<dnd::Campaign> loaded = dnd::ReadCampaign(path);
  Expect(loaded.has_value() && loaded->scenes.size() == 2 &&
             loaded->active == 1 && loaded->scenes.back().name == "Вторая",
         "the background write produces a readable campaign");

  campaign.active = 0;
  saver.Request(campaign, path);
  Expect(saver.Flush(), "Flush reports the last write succeeded");
  Expect(!saver.Busy(), "Flush leaves the saver idle");
  const std::optional<dnd::Campaign> newer = dnd::ReadCampaign(path);
  Expect(newer.has_value() && newer->active == 0,
         "the latest request wins on disk");

  saver.Request(campaign, std::filesystem::temp_directory_path() /
                              "dnd_no_such_dir" / "campaign.dndcamp");
  Expect(!saver.Flush(), "a failed write is reported");
  Expect(!saver.TakeResult().has_value(), "Flush consumes the result");

  // Деструктор дописывает уже поставленную просьбу, а не теряет её.
  {
    dnd::CampaignSaver last;
    last.Request(campaign, path);
  }
  Expect(dnd::ReadCampaign(path).has_value(),
         "the destructor finishes a queued write");
  std::filesystem::remove(path, cleanup_error);
}

// Стабильные id сцен: имя и индекс меняются, а оверлей партии ссылается на id.
auto TestSceneIds() -> void {
  dnd::Campaign campaign = dnd::MakeCampaign(dnd::GameSnapshot{}, "Первая");
  Expect(campaign.scenes.front().id == 1 && campaign.next_scene_id == 2,
         "a new campaign numbers its first scene");
  const std::optional<dnd::Scene> copy = dnd::CopyScene(campaign, 0);
  Expect(copy.has_value() && copy->id == 2 && copy->name == "Первая" &&
             !copy->overlay_only,
         "a scene copy gets a new id and keeps the name");
  if (!copy.has_value()) {
    return;
  }
  campaign.scenes.push_back(*copy);
  Expect(campaign.next_scene_id == 3, "the scene id counter moves forward");
  Expect(dnd::FindSceneIndex(campaign, 2) == 1, "a scene is found by id");
  Expect(dnd::FindSceneIndex(campaign, 99) == dnd::kNoScene,
         "an unknown scene id is not found");
  Expect(!dnd::CopyScene(campaign, 5).has_value(),
         "copying a missing scene is refused");
  campaign.scenes.erase(campaign.scenes.begin() + 1);
  Expect(dnd::NextSceneId(campaign) == 3, "a deleted scene id is not reused");

  dnd::Overlay overlay;
  const dnd::SceneId first = dnd::NextOverlaySceneId(overlay);
  const dnd::SceneId second = dnd::NextOverlaySceneId(overlay);
  Expect(first == dnd::kOverlaySceneIdBase &&
             second == dnd::kOverlaySceneIdBase + 1,
         "party scene ids live in their own range");
}

// Слияние: записи партии поверх кампании по id, база не меняется.
auto TestOverlayMerge() -> void {
  const dnd::GameSnapshot plain{.map = dnd::GridMap(4, 4), .next_token_id = 1};
  dnd::Campaign base = dnd::MakeCampaign(plain, "Пролог");
  base.scenes.push_back(dnd::Scene{
      .id = dnd::NextSceneId(base), .name = "Пещера", .snapshot = plain});
  base.scenes.push_back(dnd::Scene{
      .id = dnd::NextSceneId(base), .name = "Финал", .snapshot = plain});
  base.active = 1;
  const dnd::SceneId prologue = base.scenes[0].id;
  const dnd::SceneId cave = base.scenes[1].id;
  const dnd::SceneId finale = base.scenes[2].id;

  dnd::Overlay overlay;
  overlay.campaign_fingerprint = dnd::CampaignFingerprint(base);
  dnd::GameSnapshot played = plain;
  played.game_time_seconds = 600;
  Expect(dnd::SetSceneSnapshot(overlay, prologue, played),
         "a played scene gets an overlay snapshot");
  Expect(dnd::SetSceneName(overlay, cave, "Глубина"),
         "an overlay renames a campaign scene");
  Expect(dnd::SetSceneNote(overlay, cave, "Заметка партии"),
         "an overlay stores a scene note");
  const dnd::SceneId added = dnd::NextOverlaySceneId(overlay);
  dnd::GameSnapshot added_snapshot = plain;
  added_snapshot.game_time_seconds = 120;
  Expect(dnd::AddScene(overlay, added, "Новая", added_snapshot),
         "a scene created in the party is added");
  dnd::SetActive(overlay, added);

  const dnd::OverlayMerge merged = dnd::ApplyOverlay(base, overlay);
  Expect(!merged.rolled_back && merged.campaign.scenes.size() == 4,
         "the merge keeps every scene");
  Expect(base.scenes.size() == 3 && base.scenes[0].name == "Пролог" &&
             base.scenes[0].snapshot.game_time_seconds == 0 &&
             base.scenes[1].name == "Пещера" && base.active == 1,
         "the merge does not touch the campaign");
  const std::size_t played_index =
      dnd::FindSceneIndex(merged.campaign, prologue);
  Expect(played_index != dnd::kNoScene &&
             merged.campaign.scenes[played_index].snapshot.game_time_seconds ==
                 600,
         "the played snapshot wins over the campaign");
  const std::size_t renamed_index = dnd::FindSceneIndex(merged.campaign, cave);
  Expect(renamed_index != dnd::kNoScene &&
             merged.campaign.scenes[renamed_index].name == "Глубина" &&
             merged.campaign.scenes[renamed_index].note == "Заметка партии",
         "the overlay name and note win");
  const std::size_t party_index = dnd::FindSceneIndex(merged.campaign, added);
  Expect(party_index != dnd::kNoScene &&
             merged.campaign.scenes[party_index].overlay_only &&
             merged.campaign.active == party_index,
         "a party scene is marked as missing from the campaign");

  // Tombstone скрывает базовую сцену; активной становится следующая.
  Expect(dnd::RemoveScene(overlay, cave),
         "a scene can be deleted in the party");
  dnd::SetActive(overlay, 0);
  const dnd::OverlayMerge without_cave = dnd::ApplyOverlay(base, overlay);
  Expect(dnd::FindSceneIndex(without_cave.campaign, cave) == dnd::kNoScene &&
             without_cave.campaign.scenes.size() == 3,
         "a tombstone hides the campaign scene");
  Expect(without_cave.campaign.active ==
             dnd::FindSceneIndex(without_cave.campaign, finale),
         "deleting the active scene moves to the next one");

  // Сцена, удалённая в редакторе, но сыгранная в партии, не теряется.
  dnd::Campaign edited = base;
  edited.scenes.erase(edited.scenes.begin());
  edited.active = 0;
  const dnd::OverlayMerge rescued = dnd::ApplyOverlay(edited, overlay);
  const std::size_t rescued_index =
      dnd::FindSceneIndex(rescued.campaign, prologue);
  Expect(
      rescued_index != dnd::kNoScene &&
          rescued.campaign.scenes[rescued_index].overlay_only &&
          rescued.campaign.scenes[rescued_index].snapshot.game_time_seconds ==
              600,
      "a scene deleted in the editor keeps the party progress");

  // Дубль имени после слияния получает суффикс, кампания не меняется.
  dnd::Overlay clash;
  Expect(dnd::SetSceneName(clash, cave, "Пролог"),
         "an overlay may ask for a taken name");
  const dnd::OverlayMerge renamed = dnd::ApplyOverlay(base, clash);
  const std::size_t clash_index = dnd::FindSceneIndex(renamed.campaign, cave);
  Expect(clash_index != dnd::kNoScene &&
             renamed.campaign.scenes[clash_index].name == "Пролог (2)" &&
             base.scenes[0].name == "Пролог",
         "a duplicate name gets a suffix after the merge");

  // Пустой итог откатывается к кампании с предупреждением.
  dnd::Overlay empty;
  for (const dnd::Scene& scene : base.scenes) {
    Expect(dnd::RemoveScene(empty, scene.id), "a base scene can be tombstoned");
  }
  const dnd::OverlayMerge rolled = dnd::ApplyOverlay(base, empty);
  Expect(rolled.rolled_back &&
             rolled.campaign.scenes.size() == base.scenes.size() &&
             rolled.campaign.scenes[0].name == "Пролог",
         "an overlay that hides every scene falls back to the campaign");
  Expect(!dnd::SetSceneSnapshot(empty, prologue, played),
         "a deleted scene takes no snapshot");
  Expect(!dnd::AddScene(empty, prologue, "Чужая", plain),
         "a campaign id cannot be used for a party scene");

  // Номера сцен партии не переиспользуются после удаления.
  dnd::Overlay ids;
  const dnd::SceneId first = dnd::NextOverlaySceneId(ids);
  Expect(dnd::AddScene(ids, first, "А", plain), "a party scene is added");
  const dnd::SceneId second = dnd::NextOverlaySceneId(ids);
  Expect(second > first && dnd::RemoveScene(ids, first) &&
             dnd::NextOverlaySceneId(ids) > second,
         "deleted party scene ids are not reused");
}

// Удаление активной сцены в партии: соседняя становится активной, её id
// переживает перезапуск, а последнюю сцену объединённого списка не удалить.
auto TestPartySceneDelete() -> void {
  const dnd::GameSnapshot plain{.map = dnd::GridMap(4, 4), .next_token_id = 1};
  dnd::Campaign base = dnd::MakeCampaign(plain, "Пролог");
  base.scenes.push_back(dnd::Scene{
      .id = dnd::NextSceneId(base), .name = "Пещера", .snapshot = plain});
  base.scenes.push_back(dnd::Scene{
      .id = dnd::NextSceneId(base), .name = "Финал", .snapshot = plain});
  base.active = 1;
  const dnd::SceneId cave = base.scenes[1].id;

  dnd::Overlay overlay = dnd::FreshOverlay(base);
  dnd::Campaign merged = dnd::ApplyOverlay(base, overlay).campaign;
  Expect(merged.scenes.size() == 3 && merged.active == 1,
         "the party starts on the campaign's active scene");
  for (const dnd::Scene& scene : merged.scenes) {
    Expect(dnd::SetSceneSnapshot(overlay, scene.id, scene.snapshot),
           "a played scene is recorded");
  }

  Expect(dnd::DeletePartyScene(merged, overlay, merged.active),
         "the active scene is deleted in the party");
  const dnd::SceneId neighbour = merged.scenes[merged.active].id;
  Expect(merged.scenes.size() == 2 && neighbour != cave &&
             dnd::FindSceneIndex(merged, cave) == dnd::kNoScene,
         "the deleted scene leaves the merged list");
  Expect(overlay.active == neighbour,
         "the neighbouring scene id is stored in the overlay");

  // id активной сцены доезжает до файла партии и восстанавливает выбор.
  const std::filesystem::path path = std::filesystem::temp_directory_path() /
                                     "dnd_core_tests_party_delete.dndoverlay";
  Expect(dnd::WriteOverlay(overlay, path), "the party is written");
  const std::optional<dnd::Overlay> loaded = dnd::ReadOverlay(path);
  Expect(loaded.has_value() && loaded->active == neighbour,
         "the neighbouring id survives the restart");
  if (loaded.has_value()) {
    const dnd::OverlayMerge reopened = dnd::ApplyOverlay(base, *loaded);
    Expect(reopened.campaign.active ==
               dnd::FindSceneIndex(reopened.campaign, neighbour),
           "the reopened party stays on the neighbouring scene");
  }
  std::filesystem::remove(path);

  // Сцена, созданная в партии, удаляется из записей оверлея.
  const dnd::SceneId added = dnd::NextOverlaySceneId(overlay);
  Expect(dnd::AddScene(overlay, added, "Новая", plain),
         "a party scene is added");
  merged.scenes.push_back(
      dnd::Scene{.id = added, .name = "Новая", .snapshot = plain});
  merged.active = merged.scenes.size() - 1;
  Expect(dnd::DeletePartyScene(merged, overlay, merged.active) &&
             dnd::FindOverlayScene(overlay, added) == nullptr,
         "a party scene is deleted without a tombstone");
  Expect(overlay.active == merged.scenes[merged.active].id,
         "the party scene deletion stores the new active id");

  // Последнюю сцену объединённого списка удалить нельзя.
  while (merged.scenes.size() > 1) {
    Expect(dnd::DeletePartyScene(merged, overlay, merged.active),
           "scenes are deleted down to the last one");
  }
  Expect(merged.scenes.size() == 1 && !dnd::CanDeleteScene(merged) &&
             !dnd::DeletePartyScene(merged, overlay, 0) &&
             merged.scenes.size() == 1,
         "the last scene of the merged list stays");
}

// Файл партии: DNDO, строгий декодер, атомарная запись.
auto TestOverlayRoundtrip() -> void {
  const dnd::GameSnapshot plain{.map = dnd::GridMap(3, 3), .next_token_id = 1};
  dnd::Overlay overlay;
  overlay.campaign_fingerprint = 0x123456789ABCDEF0ULL;
  const dnd::SceneId added = dnd::NextOverlaySceneId(overlay);
  Expect(dnd::AddScene(overlay, added, "Новая", plain),
         "a party scene is encoded");
  Expect(dnd::SetSceneName(overlay, 7, "Имя") &&
             dnd::SetSceneNote(overlay, 7, "Заметка") &&
             dnd::RemoveScene(overlay, 8),
         "name, note and tombstone records are encoded");
  dnd::SetActive(overlay, added);

  const std::vector<std::uint8_t> payload =
      dnd::EncodeOverlayAs(overlay, dnd::kOverlayVersion);
  Expect(!payload.empty(), "an overlay encodes");
  Expect(dnd::EncodeOverlayAs(overlay, 0).empty(),
         "an unknown overlay version is not encoded");

  constexpr std::uint32_t overlay_magic = 0x4F444E44U;
  const std::filesystem::path path = std::filesystem::temp_directory_path() /
                                     "dnd_core_tests_overlay.dndoverlay";
  Expect(dnd::OverlayPath(std::filesystem::path("campaign.dndcamp")) ==
             std::filesystem::path("campaign.dndoverlay"),
         "the party file sits next to the campaign with its own extension");
  WriteVersionedFile(path, overlay_magic, dnd::kOverlayVersion, payload);
  const std::optional<dnd::Overlay> loaded = dnd::ReadOverlay(path);
  Expect(loaded.has_value(), "an overlay reads back");
  if (loaded.has_value()) {
    Expect(loaded->campaign_fingerprint == overlay.campaign_fingerprint,
           "the campaign fingerprint survives the roundtrip");
    Expect(loaded->active == added &&
               loaded->next_scene_id == overlay.next_scene_id,
           "the active scene and the id counter survive the roundtrip");
    Expect(loaded->scenes.size() == overlay.scenes.size(),
           "every overlay record survives the roundtrip");
    const dnd::OverlayScene* named = dnd::FindOverlayScene(*loaded, 7);
    Expect(named != nullptr && named->name == "Имя" &&
               named->note == "Заметка" && !named->deleted &&
               !named->snapshot.has_value(),
           "an overlay name and note survive the roundtrip");
    const dnd::OverlayScene* tombstone = dnd::FindOverlayScene(*loaded, 8);
    Expect(tombstone != nullptr && tombstone->deleted &&
               !tombstone->snapshot.has_value(),
           "a tombstone survives the roundtrip");
    const dnd::OverlayScene* party = dnd::FindOverlayScene(*loaded, added);
    Expect(party != nullptr && party->snapshot.has_value() &&
               party->snapshot->map.Width() == 3,
           "a party scene snapshot survives the roundtrip");
  }
  std::filesystem::remove(path);

  std::vector<std::uint8_t> unknown_flag = payload;
  // Запись 0: id (8 байт) идёт после отпечатка, active, next и счётчика.
  constexpr std::size_t first_flags = 3 * 8 + 4 + 8;
  unknown_flag[first_flags] |= 0x10;
  WriteVersionedFile(path, overlay_magic, dnd::kOverlayVersion, unknown_flag);
  Expect(!dnd::ReadOverlay(path).has_value(),
         "an unknown record flag is rejected");

  std::vector<std::uint8_t> mixed_flags = payload;
  mixed_flags[first_flags] = 0x01 | 0x02;
  WriteVersionedFile(path, overlay_magic, dnd::kOverlayVersion, mixed_flags);
  Expect(!dnd::ReadOverlay(path).has_value(),
         "a tombstone with fields is rejected");

  const std::vector<std::uint8_t> truncated(payload.begin(), payload.end() - 1);
  WriteVersionedFile(path, overlay_magic, dnd::kOverlayVersion, truncated);
  Expect(!dnd::ReadOverlay(path).has_value(),
         "a truncated overlay is rejected");
  WriteVersionedFile(path, overlay_magic, dnd::kOverlayVersion + 1, payload);
  Expect(!dnd::ReadOverlay(path).has_value(),
         "an overlay from a newer version is rejected");
  std::filesystem::remove(path);

  const std::filesystem::path written =
      std::filesystem::temp_directory_path() /
      "dnd_core_tests_overlay_write.dndoverlay";
  Expect(dnd::WriteOverlay(overlay, written), "the overlay is written");
  std::filesystem::path temp = written;
  temp += ".tmp";
  Expect(!std::filesystem::exists(temp),
         "the atomic write leaves no temporary file");
  Expect(dnd::ReadOverlay(written).has_value(),
         "the written overlay reads back");
  std::filesystem::remove(written);
}

// Партия не меняет авторский файл кампании ни в памяти, ни на диске.
auto TestCampaignUntouched() -> void {
  const dnd::GameSnapshot plain{.map = dnd::GridMap(5, 5), .next_token_id = 1};
  const dnd::Campaign campaign = dnd::MakeCampaign(plain, "Шаблон");
  const std::filesystem::path campaign_path =
      std::filesystem::temp_directory_path() /
      "dnd_core_tests_untouched.dndcamp";
  Expect(dnd::WriteCampaign(campaign, campaign_path),
         "the campaign is written before the party");
  const auto read_bytes =
      [](const std::filesystem::path& path) -> std::vector<std::uint8_t> {
    std::ifstream in(path, std::ios::binary);
    std::vector<std::uint8_t> bytes;
    bytes.assign(std::istreambuf_iterator<char>(in),
                 std::istreambuf_iterator<char>());
    return bytes;
  };
  const std::vector<std::uint8_t> before = read_bytes(campaign_path);

  dnd::Overlay overlay;
  overlay.campaign_fingerprint = dnd::CampaignFingerprint(campaign);
  const dnd::SceneId id = campaign.scenes.front().id;
  Expect(dnd::SetSceneName(overlay, id, "Партия") &&
             dnd::SetSceneSnapshot(overlay, id, plain),
         "the party changes its own copy of the scene");
  const std::filesystem::path overlay_path =
      std::filesystem::temp_directory_path() /
      "dnd_core_tests_untouched.dndoverlay";
  Expect(dnd::WriteOverlay(overlay, overlay_path),
         "the party is written next to the campaign");
  Expect(read_bytes(campaign_path) == before,
         "writing the party does not touch the campaign file");

  const dnd::OverlayMerge merged = dnd::ApplyOverlay(campaign, overlay);
  Expect(merged.campaign.scenes.front().name == "Партия" &&
             campaign.scenes.front().name == "Шаблон",
         "the merge leaves the campaign object unchanged");
  std::filesystem::remove(campaign_path);
  std::filesystem::remove(overlay_path);
}

// Партия считается изменённой, только когда в ней есть записи сцен: пустой
// оверлей и смена активной сцены сами по себе — ещё не продолжение.
auto TestOverlayDirty() -> void {
  const dnd::GameSnapshot plain{.map = dnd::GridMap(3, 3), .next_token_id = 1};
  const dnd::Campaign campaign = dnd::MakeCampaign(plain, "Шаблон");
  dnd::Overlay overlay = dnd::FreshOverlay(campaign);
  Expect(overlay.campaign_fingerprint == dnd::CampaignFingerprint(campaign) &&
             overlay.next_scene_id == dnd::kOverlaySceneIdBase,
         "a fresh party remembers the campaign and starts its own ids");
  Expect(!dnd::HasOverlayChanges(overlay), "a fresh party has no changes");
  dnd::SetActive(overlay, campaign.scenes.front().id);
  Expect(!dnd::HasOverlayChanges(overlay),
         "the active scene alone is not a party change");

  const dnd::SceneId id = campaign.scenes.front().id;
  Expect(dnd::SetSceneSnapshot(overlay, id, plain),
         "a played scene is recorded");
  Expect(dnd::HasOverlayChanges(overlay),
         "a recorded scene makes the party dirty");
  Expect(dnd::ClearScene(overlay, id), "a scene record is cleared");
  Expect(!dnd::HasOverlayChanges(overlay),
         "clearing the only record makes the party clean again");
  Expect(!dnd::ClearScene(overlay, id), "a missing record is not cleared");

  const dnd::SceneId added = dnd::NextOverlaySceneId(overlay);
  Expect(dnd::AddScene(overlay, added, "Новая", plain) &&
             dnd::HasOverlayChanges(overlay),
         "a party scene makes the party dirty");
  Expect(dnd::RemoveScene(overlay, added) && !dnd::HasOverlayChanges(overlay),
         "removing a party scene leaves no record behind");
}

// Партия уходит на диск в фоновом потоке: кадр не ждёт, а новая просьба
// заменяет ещё не начатую запись.
auto TestOverlaySaver() -> void {
  const dnd::GameSnapshot plain{.map = dnd::GridMap(4, 4), .next_token_id = 1};
  const dnd::Campaign campaign = dnd::MakeCampaign(plain, "Фон");
  dnd::Overlay overlay = dnd::FreshOverlay(campaign);
  const dnd::SceneId id = campaign.scenes.front().id;
  Expect(dnd::SetSceneSnapshot(overlay, id, plain),
         "the party has something to write");
  dnd::SetActive(overlay, id);

  const std::filesystem::path path = std::filesystem::temp_directory_path() /
                                     "dnd_core_tests_overlay_saver.dndoverlay";
  std::error_code cleanup_error;
  std::filesystem::remove(path, cleanup_error);

  dnd::OverlaySaver saver;
  Expect(!saver.Busy(), "an overlay saver starts idle");
  saver.Request(overlay, path);
  Expect(saver.Busy(), "a request keeps the saver busy until the disk answers");
  Expect(!saver.TakeResult().has_value(),
         "nothing is reported before the write finishes");

  int waited = 0;
  while (saver.Busy() && waited < 2000) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    ++waited;
  }
  const std::optional<bool> result = saver.TakeResult();
  Expect(result.has_value() && *result, "background party save succeeds");
  Expect(!saver.TakeResult().has_value(), "a result is reported only once");
  const std::optional<dnd::Overlay> loaded = dnd::ReadOverlay(path);
  Expect(
      loaded.has_value() && loaded->active == id && loaded->scenes.size() == 1,
      "the background write produces a readable party");

  overlay.active = 0;
  saver.Request(overlay, path);
  Expect(saver.Flush(), "Flush reports the last party write succeeded");
  Expect(!saver.Busy(), "Flush leaves the overlay saver idle");
  const std::optional<dnd::Overlay> newer = dnd::ReadOverlay(path);
  Expect(newer.has_value() && newer->active == 0,
         "the latest party request wins on disk");

  saver.Request(overlay, std::filesystem::temp_directory_path() /
                             "dnd_no_such_dir" / "party.dndoverlay");
  Expect(!saver.Flush(), "a failed party write is reported");
  Expect(!saver.TakeResult().has_value(), "Flush consumes the party result");

  // Смена кампании после ошибки: повторная запись свежей партии доходит до
  // диска, поэтому переход можно разрешать.
  saver.Request(overlay, path);
  Expect(saver.Flush() && dnd::ReadOverlay(path).has_value(),
         "a retry after a failed party write reaches the disk");

  {
    dnd::OverlaySaver last;
    last.Request(overlay, path);
  }
  Expect(dnd::ReadOverlay(path).has_value(),
         "the overlay saver destructor finishes a queued write");
  std::filesystem::remove(path, cleanup_error);
}

// Испорченные байты — обычное дело при обрыве связи или битом файле.
// Декодеры
// должны отвечать отказом, а не падать и не читать за границей буфера; случайно
// принятые данные обязаны переживать повторное кодирование.
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
      .snapshot = server.ClientSnapshot("alice"),
      .fog = server.FogState("alice"),
  });
  Expect(view.Log().size() == 1, "view refresh keeps the log history");

  view.AddLocalLine("local only");
  Expect(view.Log().size() == 2 && view.Log().back().text == "local only" &&
             view.Log().back().author.empty() &&
             view.Log().back().timestamp != 0,
         "local feedback stays in the client log");
  Expect(view.AppendedLogLines() == 2,
         "local feedback is counted for the session log");

  view.ClearLog();
  Expect(view.Log().empty(), "a new party starts with an empty log");
  Expect(view.AppendedLogLines() == 2,
         "clearing the log keeps the session log counter");
  view.AddLocalLine("fresh party");
  Expect(view.Log().size() == 1 && view.Log().back().text == "fresh party",
         "the first line after clearing opens a new log");
  Expect(view.AppendedLogLines() == 3,
         "cleared lines are not counted twice for the session log");
}

auto TestChatCommandParsing() -> void {
  using dnd::ChatCommandError;
  using dnd::ChatCommandKind;
  const auto parse = [](std::string_view text) -> dnd::ChatCommand {
    return dnd::ParseChatCommand(text);
  };

  const dnd::ChatCommand say = parse("привет столу");
  Expect(say.kind == ChatCommandKind::kSay, "plain text stays chat");
  Expect(say.error == ChatCommandError::kNone, "plain text parses cleanly");
  Expect(say.text == "привет столу", "plain text is kept");
  Expect(parse("/неизвестно").kind == ChatCommandKind::kSay,
         "unknown slash command stays chat");

  // /roll
  const dnd::ChatCommand roll = parse("/roll adv d20+5");
  Expect(roll.kind == ChatCommandKind::kRoll, "/roll is a roll");
  Expect(roll.text == "adv d20+5", "/roll keeps the expression");
  Expect(parse("/roll 2d6+3").error == ChatCommandError::kNone,
         "/roll accepts plain dice");
  Expect(parse("/roll чушь").error == ChatCommandError::kBadRoll,
         "/roll rejects a broken expression");
  Expect(parse("/roll").error == ChatCommandError::kBadRoll,
         "/roll without an expression is an error");

  // /kick
  const dnd::ChatCommand kick = parse("/kick bob");
  Expect(kick.kind == ChatCommandKind::kKick, "/kick is recognized");
  Expect(kick.text == "bob", "/kick keeps the player name");
  Expect(parse("/kick ").error == ChatCommandError::kBadKick,
         "/kick without a name is an error");

  // /owner
  const dnd::ChatCommand owner = parse("/owner bob");
  Expect(owner.kind == ChatCommandKind::kOwner, "/owner is recognized");
  Expect(owner.text == "bob", "/owner keeps the owner");
  const dnd::ChatCommand owner_clear = parse("/owner");
  Expect(owner_clear.kind == ChatCommandKind::kOwner &&
             owner_clear.text.empty() &&
             owner_clear.error == ChatCommandError::kNone,
         "/owner without a name clears ownership");

  // /scene
  Expect(parse("/scene").kind == ChatCommandKind::kSceneList,
         "/scene opens the list");
  Expect(parse("/scene new").kind == ChatCommandKind::kSceneNew,
         "/scene new copies the scene");
  const dnd::ChatCommand scene_rename = parse("/scene rename Старый Новый");
  Expect(scene_rename.kind == ChatCommandKind::kSceneRename,
         "/scene rename is recognized");
  Expect(scene_rename.text == " Старый Новый",
         "/scene rename keeps both names");
  const dnd::ChatCommand scene_switch = parse("/scene Пещера");
  Expect(scene_switch.kind == ChatCommandKind::kSceneSwitch,
         "/scene NAME switches");
  Expect(scene_switch.text == "Пещера", "/scene keeps the target name");

  // /rename
  const dnd::ChatCommand rename = parse("/rename Гоблин вожак");
  Expect(rename.kind == ChatCommandKind::kRename, "/rename is recognized");
  Expect(rename.text == "Гоблин вожак", "/rename keeps the spaces");
  Expect(parse("/rename ").error == ChatCommandError::kBadRename,
         "/rename without a name is an error");

  // /maxhp
  const dnd::ChatCommand max_hp = parse("/maxhp 24");
  Expect(max_hp.kind == ChatCommandKind::kMaxHp, "/maxhp is recognized");
  Expect(max_hp.value == 24, "/maxhp keeps the value");
  Expect(parse("/maxhp 0").error == ChatCommandError::kBadMaxHp,
         "/maxhp rejects zero");
  Expect(parse("/maxhp много").error == ChatCommandError::kBadMaxHp,
         "/maxhp rejects text");

  // /damage и /heal делят одну подсказку.
  const dnd::ChatCommand damage = parse("/damage 7");
  Expect(damage.kind == ChatCommandKind::kDamage && damage.value == 7,
         "/damage keeps the amount");
  const dnd::ChatCommand heal = parse("/heal 5");
  Expect(heal.kind == ChatCommandKind::kHeal && heal.value == 5,
         "/heal keeps the amount");
  Expect(parse("/damage -1").error == ChatCommandError::kBadDamage,
         "/damage rejects a negative amount");
  const dnd::ChatCommand typed_damage = parse("/damage 9 огонь");
  Expect(typed_damage.kind == ChatCommandKind::kDamage &&
             typed_damage.value == 9 &&
             typed_damage.damage_type == dnd::DamageType::kFire,
         "/damage keeps the damage type");
  Expect(parse("/damage 9 огонx").error == ChatCommandError::kBadDamage,
         "/damage rejects an unknown damage type");
  Expect(parse("/heal 0").error == ChatCommandError::kBadDamage,
         "/heal rejects zero");

  // /aoe
  const dnd::ChatCommand aoe = parse("/aoe 2d6 15");
  Expect(aoe.kind == ChatCommandKind::kAoe, "/aoe is recognized");
  Expect(aoe.text == "2d6" && aoe.save_dc == 15,
         "/aoe keeps the damage and the DC");
  const dnd::ChatCommand typed_aoe = parse("/aoe 2d6 15 холод");
  Expect(typed_aoe.text == "2d6" && typed_aoe.save_dc == 15 &&
             typed_aoe.damage_type == dnd::DamageType::kCold,
         "/aoe keeps the damage type");
  Expect(parse("/aoe 2d6").error == ChatCommandError::kBadAoe,
         "/aoe without a DC is an error");
  Expect(parse("/aoe 2d6 200").error == ChatCommandError::kBadAoe,
         "/aoe rejects an impossible DC");

  // /portrait
  const dnd::ChatCommand portrait = parse("/portrait hero.png");
  Expect(portrait.kind == ChatCommandKind::kPortrait,
         "/portrait is recognized");
  Expect(portrait.text == "hero.png", "/portrait keeps the file name");
  const dnd::ChatCommand portrait_off = parse("/portrait off");
  Expect(portrait_off.kind == ChatCommandKind::kPortrait &&
             portrait_off.text.empty(),
         "/portrait off clears the picture");
  Expect(
      parse("/portrait ../secret.png").error == ChatCommandError::kBadPortrait,
      "/portrait rejects a path");

  // /map
  Expect(parse("/map off").kind == ChatCommandKind::kMapOff, "/map off clears");
  const dnd::ChatCommand opacity = parse("/map opacity 60");
  Expect(opacity.kind == ChatCommandKind::kMapOpacity && opacity.value == 60,
         "/map opacity keeps the value");
  const dnd::ChatCommand scale = parse("/map scale 150");
  Expect(scale.kind == ChatCommandKind::kMapScale && scale.value == 150,
         "/map scale keeps the value");
  const dnd::ChatCommand image = parse("/map cave.png");
  Expect(image.kind == ChatCommandKind::kMapImage && image.text == "cave.png",
         "/map keeps the file name");
  Expect(parse("/map ../../cave.png").error == ChatCommandError::kBadMap,
         "/map rejects a path");
  Expect(parse("/map opacity 300").error == ChatCommandError::kBadMap,
         "/map rejects an impossible opacity");
  Expect(parse("/map").error == ChatCommandError::kBadMap,
         "/map without arguments is an error");

  // /reveal
  Expect(parse("/reveal").kind == ChatCommandKind::kReveal,
         "/reveal opens the area under the cursor");
  Expect(parse("/reveal off").kind == ChatCommandKind::kRevealOff,
         "/reveal off clears manual reveals");
  Expect(parse("/unreveal").kind == ChatCommandKind::kUnreveal,
         "/unreveal closes the area under the cursor");
  Expect(parse("/unreveal всё").error == ChatCommandError::kBadReveal,
         "/unreveal rejects unknown arguments");
  Expect(parse("/reveal всё").error == ChatCommandError::kBadReveal,
         "/reveal rejects unknown arguments");

  // /xp
  const dnd::ChatCommand xp = parse("/xp 300");
  Expect(xp.kind == ChatCommandKind::kXp && xp.value == 300 && !xp.party,
         "/xp keeps the amount for one character");
  const dnd::ChatCommand xp_party = parse("/xp 300 all");
  Expect(xp_party.kind == ChatCommandKind::kXp && xp_party.value == 300 &&
             xp_party.party,
         "/xp all awards the whole party");
  Expect(parse("/xp").error == ChatCommandError::kBadXp,
         "/xp without an amount is an error");
  Expect(parse("/xp 300 some").error == ChatCommandError::kBadXp,
         "/xp rejects an unknown target");
  Expect(parse("/xp -5").error == ChatCommandError::kBadXp,
         "/xp rejects negative experience");

  // Короткие флаги.
  Expect(parse("/claim").kind == ChatCommandKind::kClaim,
         "/claim is a command");
  Expect(parse("/ping").kind == ChatCommandKind::kPing, "/ping is a command");
  Expect(parse("/deathsave").kind == ChatCommandKind::kDeathSave,
         "/deathsave is a command");
  const dnd::ChatCommand rest = parse("/rest");
  Expect(
      rest.kind == ChatCommandKind::kRest && !rest.long_rest && rest.value == 0,
      "/rest is a short rest that spends dice until full");
  const dnd::ChatCommand diced_rest = parse("/rest 2");
  Expect(diced_rest.kind == ChatCommandKind::kRest && diced_rest.value == 2,
         "/rest N limits the hit dice");
  Expect(parse("/rest 0").error == ChatCommandError::kBadRest,
         "/rest rejects zero dice");
  Expect(parse("/rest много").error == ChatCommandError::kBadRest,
         "/rest rejects text");
  const dnd::ChatCommand long_rest = parse("/longrest");
  Expect(long_rest.kind == ChatCommandKind::kRest && long_rest.long_rest,
         "/longrest is a long rest");
  Expect(parse("/longrest 2").error == ChatCommandError::kBadRest,
         "/longrest does not take a dice count");
  const dnd::ChatCommand fall = parse("/fall 30");
  Expect(fall.kind == ChatCommandKind::kFall && fall.value == 30,
         "/fall keeps the height");
  Expect(parse("/fall 0").error == ChatCommandError::kBadFall,
         "/fall rejects zero height");
  Expect(parse("/fall высоко").error == ChatCommandError::kBadFall,
         "/fall rejects text");

  // /time: по умолчанию минуты, суффиксы s/m/h.
  const dnd::ChatCommand minutes = parse("/time 10");
  Expect(minutes.kind == ChatCommandKind::kTime && minutes.value == 600,
         "/time defaults to minutes");
  Expect(parse("/time 30s").value == 30, "/time accepts seconds");
  Expect(parse("/time 2h").value == 7200, "/time accepts hours");
  Expect(parse("/time").error == ChatCommandError::kBadTime,
         "/time without an interval is an error");
  Expect(parse("/time 10x").error == ChatCommandError::kBadTime,
         "/time rejects unknown suffixes");
  Expect(parse("/time 0").error == ChatCommandError::kBadTime,
         "/time rejects zero");
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

auto TestSessionLogHistory() -> void {
  const std::string content =
      "\n=== сессия 2026-11-20 18:40:12 ===\n"
      "[2026-11-20 18:40:13] alice: привет\n"
      "[2026-11-20 18:40:14] bob: бросок d20\n"
      "без времени\n"
      "\n=== сессия 2026-11-21 10:00:00 ===\n"
      "[2026-11-21 10:00:01] DM: Огонь по площади\n";
  const std::vector<dnd::HistoryLine> lines =
      dnd::ParseSessionLog(content, 100);
  Expect(lines.size() == 6, "session history parses every non-empty line");
  Expect(lines[0].header && lines[0].time == "2026-11-20 18:40:12",
         "session history keeps the session header");
  Expect(lines[1].time == "2026-11-20 18:40:13" && lines[1].author == "alice" &&
             lines[1].text == "привет",
         "session history splits time and author");
  Expect(!lines[3].header && lines[3].author.empty() &&
             lines[3].text == "без времени",
         "session history keeps untimed system lines");
  Expect(lines[4].header && lines[5].author == "DM",
         "session history parses later sessions");

  std::vector<std::size_t> found = dnd::SearchSessionLog(lines, "ПРИВЕТ");
  Expect(found == std::vector<std::size_t>{0, 1},
         "search is case-insensitive and keeps the session header");
  found = dnd::SearchSessionLog(lines, "огонь");
  Expect(found == std::vector<std::size_t>{4, 5},
         "search finds a line from a past session");
  found = dnd::SearchSessionLog(lines, "d20");
  Expect(found == std::vector<std::size_t>{0, 2},
         "search skips sessions without matches");
  Expect(dnd::SearchSessionLog(lines, "abc").empty(),
         "search returns nothing for an absent word");
  Expect(dnd::SearchSessionLog(lines, "").size() == lines.size(),
         "empty search shows the whole journal");

  const std::vector<dnd::HistoryLine> tail = dnd::ParseSessionLog(content, 2);
  Expect(tail.size() == 2 && tail.back().text == "Огонь по площади",
         "history keeps only the requested tail");

  const std::filesystem::path path =
      std::filesystem::temp_directory_path() / "dnd_core_tests_history.log";
  {
    std::ofstream out(path, std::ios::binary);
    out << content;
  }
  const std::vector<dnd::HistoryLine> loaded =
      dnd::LoadSessionLog(path.string(), 100);
  Expect(loaded.size() == lines.size() && loaded[1].text == "привет",
         "history reader loads a session log file");
  Expect(dnd::LoadSessionLog(path.string() + ".missing", 100).empty(),
         "missing session log loads as empty");
  std::filesystem::remove(path);
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
    Expect(server.EventVisibleTo(ping.events.front(), "alice"),
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
  view.Apply(dnd::ViewUpdated{.snapshot = server.ClientSnapshot("alice"),
                              .fog = server.FogState("alice")});
  Expect(view.Pings().empty(), "world refresh clears pings");
}

auto TestMapBackground() -> void {
  Expect(dnd::IsSafeImageName("dungeon.png"), "a plain png name is safe");
  Expect(dnd::IsSafeImageName("Dungeon.JPG"),
         "extensions are case-insensitive");
  Expect(dnd::IsSafeImageName("map-1.jpeg"),
         "hyphens are allowed in image names");
  Expect(!dnd::IsSafeImageName(""), "an empty image name is unsafe");
  Expect(!dnd::IsSafeImageName("../secret.png"),
         "parent directory escapes are rejected");
  Expect(!dnd::IsSafeImageName("maps/dungeon.png"), "directories are rejected");
  Expect(!dnd::IsSafeImageName("C:\\dungeon.png"),
         "windows paths are rejected");
  Expect(!dnd::IsSafeImageName("dungeon.exe"),
         "unknown extensions are rejected");
  Expect(!dnd::IsSafeImageName(std::string(200, 'a') + ".png"),
         "overlong image names are rejected");

  Expect(dnd::IsValidMapBackground(dnd::MapBackground{}),
         "no image with default opacity and scale is valid");
  Expect(dnd::IsValidMapBackground(dnd::MapBackground{
             .image = "dungeon.png", .opacity = 40, .scale = 150}),
         "a normal background is valid");
  Expect(!dnd::IsValidMapBackground(
             dnd::MapBackground{.image = "dungeon.png", .opacity = 101}),
         "opacity above 100 is rejected");
  Expect(!dnd::IsValidMapBackground(
             dnd::MapBackground{.image = "dungeon.png", .opacity = -1}),
         "negative opacity is rejected");
  Expect(!dnd::IsValidMapBackground(
             dnd::MapBackground{.image = "dungeon.png", .scale = 5}),
         "scale below the minimum is rejected");
  Expect(
      !dnd::IsValidMapBackground(dnd::MapBackground{.image = "../dungeon.png"}),
      "an unsafe image name makes the background invalid");

  // Директивы редактора повторяют хвост /map: тот же разбор и та же подложка.
  dnd::MapBackground edited;
  Expect(dnd::ApplyMapCommand(edited, dnd::ParseChatCommand("/map cave.png")) &&
             edited.image == "cave.png",
         "the editor applies an image name");
  Expect(
      dnd::ApplyMapCommand(edited, dnd::ParseChatCommand("/map opacity 60")) &&
          edited.opacity == 60 && edited.image == "cave.png",
      "the editor changes opacity and keeps the image");
  Expect(
      dnd::ApplyMapCommand(edited, dnd::ParseChatCommand("/map scale 150")) &&
          edited.scale == 150,
      "the editor changes scale");
  Expect(dnd::ApplyMapCommand(edited, dnd::ParseChatCommand("/map off")) &&
             edited.image.empty() && edited.opacity == 100 &&
             edited.scale == 100,
         "off resets the whole background");
  Expect(!dnd::ApplyMapCommand(edited, dnd::ParseChatCommand("/map ../x.png")),
         "an unsafe image is refused");
  Expect(!dnd::ApplyMapCommand(edited, dnd::ParseChatCommand("/roll d20")),
         "a non-map command changes nothing");

  const dnd::GameSnapshot snapshot{.map = dnd::GridMap(8, 6),
                                   .next_token_id = 1};
  dnd::GameServer server(snapshot, 6, 11);
  const dnd::MapBackground background{
      .image = "dungeon.png", .opacity = 45, .scale = 160};

  const dnd::CommandOutcome player_change = server.HandleCommand(
      dnd::SetMapBackground{.background = background}, "alice", false);
  Expect(!player_change.accepted, "only the DM changes the map background");
  Expect(server.State().Background().image.empty(),
         "a rejected background does not change the world");

  const dnd::CommandOutcome dm_change = server.HandleCommand(
      dnd::SetMapBackground{.background = background}, "dm", true);
  Expect(dm_change.accepted, "the DM sets the map background");
  Expect(server.State().Background().image == "dungeon.png" &&
             server.State().Background().opacity == 45 &&
             server.State().Background().scale == 160,
         "the background is stored in the world");
  Expect(dm_change.events.size() == 1 &&
             std::holds_alternative<dnd::MapBackgroundUpdated>(
                 dm_change.events.front()),
         "setting the background emits an update event");
  if (!dm_change.events.empty() &&
      std::holds_alternative<dnd::MapBackgroundUpdated>(
          dm_change.events.front())) {
    Expect(server.EventVisibleTo(dm_change.events.front(), "alice"),
           "the background update reaches players");
    dnd::ClientView view;
    view.Apply(dm_change.events.front());
    Expect(view.Background().image == "dungeon.png" &&
               view.Background().opacity == 45 &&
               view.Background().scale == 160,
           "the client view keeps the background");
  }

  const dnd::CommandOutcome invalid = server.HandleCommand(
      dnd::SetMapBackground{
          .background =
              dnd::MapBackground{.image = "dungeon.png", .opacity = 200}},
      "dm", true);
  Expect(!invalid.accepted, "an out-of-range background is rejected");

  const dnd::CommandOutcome removed = server.HandleCommand(
      dnd::SetMapBackground{.background = dnd::MapBackground{}}, "dm", true);
  Expect(removed.accepted && server.State().Background().image.empty(),
         "an empty background removes the image");

  const dnd::Command command{dnd::SetMapBackground{.background = background}};
  const std::vector<std::uint8_t> bytes = dnd::EncodeCommand(command);
  const std::optional<dnd::Command> decoded =
      dnd::DecodeCommand(bytes.data(), bytes.size());
  Expect(
      decoded.has_value() &&
          std::holds_alternative<dnd::SetMapBackground>(*decoded) &&
          std::get<dnd::SetMapBackground>(*decoded).background.image ==
              "dungeon.png" &&
          std::get<dnd::SetMapBackground>(*decoded).background.opacity == 45 &&
          std::get<dnd::SetMapBackground>(*decoded).background.scale == 160,
      "map background command survives serialization");

  const dnd::Event event{dnd::MapBackgroundUpdated{.background = background}};
  const std::vector<std::uint8_t> event_bytes = dnd::EncodeEvent(event);
  const std::optional<dnd::Event> event_decoded =
      dnd::DecodeEvent(event_bytes.data(), event_bytes.size());
  Expect(
      event_decoded.has_value() &&
          std::holds_alternative<dnd::MapBackgroundUpdated>(*event_decoded) &&
          std::get<dnd::MapBackgroundUpdated>(*event_decoded)
                  .background.image == "dungeon.png" &&
          std::get<dnd::MapBackgroundUpdated>(*event_decoded)
                  .background.scale == 160,
      "map background event survives serialization");
}

auto TestTokenPortrait() -> void {
  const dnd::GameSnapshot snapshot{
      .map = dnd::GridMap(8, 8),
      .tokens =
          {
              dnd::Token{.id = 1,
                         .name = "hero",
                         .owner = "alice",
                         .kind = dnd::TokenKind::kPlayer,
                         .cell = CellAt(1, 1)},
              dnd::Token{.id = 2,
                         .name = "goblin",
                         .owner = "dm",
                         .kind = dnd::TokenKind::kMonster,
                         .cell = CellAt(2, 1)},
          },
      .next_token_id = 3,
  };
  dnd::GameServer server(snapshot, 6, 13);

  const dnd::CommandOutcome owner_change = server.HandleCommand(
      dnd::SetTokenPortrait{.id = 1, .portrait = "hero.png"}, "alice", false);
  Expect(owner_change.accepted, "the owner sets a character portrait");
  Expect(owner_change.events.size() == 1 &&
             std::holds_alternative<dnd::TokenUpdated>(
                 owner_change.events.front()),
         "a portrait change updates the token");
  if (!owner_change.events.empty() &&
      std::holds_alternative<dnd::TokenUpdated>(owner_change.events.front())) {
    Expect(std::get<dnd::TokenUpdated>(owner_change.events.front())
                   .token.portrait == "hero.png",
           "the token keeps the portrait file name");
  }
  Expect(server.State().Tokens().front().portrait == "hero.png",
         "the world stores the portrait");

  const dnd::CommandOutcome stranger_change = server.HandleCommand(
      dnd::SetTokenPortrait{.id = 1, .portrait = "other.png"}, "bob", false);
  Expect(!stranger_change.accepted,
         "a stranger cannot change someone else's portrait");

  const dnd::CommandOutcome dm_change = server.HandleCommand(
      dnd::SetTokenPortrait{.id = 2, .portrait = "goblin.jpg"}, "dm", true);
  Expect(dm_change.accepted, "the DM changes any portrait");

  const dnd::CommandOutcome unsafe = server.HandleCommand(
      dnd::SetTokenPortrait{.id = 1, .portrait = "../secret.png"}, "dm", true);
  Expect(!unsafe.accepted, "an unsafe portrait name is rejected");

  const dnd::CommandOutcome cleared = server.HandleCommand(
      dnd::SetTokenPortrait{.id = 1, .portrait = ""}, "alice", false);
  Expect(cleared.accepted && server.State().Tokens().front().portrait.empty(),
         "an empty portrait clears the image");

  const dnd::Command command{
      dnd::SetTokenPortrait{.id = 1, .portrait = "hero.png"}};
  const std::vector<std::uint8_t> bytes = dnd::EncodeCommand(command);
  const std::optional<dnd::Command> decoded =
      dnd::DecodeCommand(bytes.data(), bytes.size());
  Expect(decoded.has_value() &&
             std::holds_alternative<dnd::SetTokenPortrait>(*decoded) &&
             std::get<dnd::SetTokenPortrait>(*decoded).id == 1 &&
             std::get<dnd::SetTokenPortrait>(*decoded).portrait == "hero.png",
         "portrait command survives serialization");

  const std::vector<std::uint8_t> unsafe_bytes{
      28,  1,   0,   0,   0,   13,  0,   0,   0,   '.', '.',
      '/', 's', 'e', 'c', 'r', 'e', 't', '.', 'p', 'n', 'g'};
  Expect(
      !dnd::DecodeCommand(unsafe_bytes.data(), unsafe_bytes.size()).has_value(),
      "an unsafe portrait is rejected by the decoder");
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

  const dnd::Command reveal_command{
      dnd::RevealArea{.cell = CellAt(2, 3), .radius = 5}};
  const std::vector<std::uint8_t> reveal_bytes =
      dnd::EncodeCommand(reveal_command);
  const std::optional<dnd::Command> reveal_decoded =
      dnd::DecodeCommand(reveal_bytes.data(), reveal_bytes.size());
  Expect(reveal_decoded.has_value() &&
             std::holds_alternative<dnd::RevealArea>(*reveal_decoded) &&
             std::get<dnd::RevealArea>(*reveal_decoded).cell == CellAt(2, 3) &&
             std::get<dnd::RevealArea>(*reveal_decoded).radius == 5,
         "reveal command survives serialization");

  const dnd::Command clear_reveal_command{dnd::ClearRevealed{}};
  const std::vector<std::uint8_t> clear_reveal_bytes =
      dnd::EncodeCommand(clear_reveal_command);
  const std::optional<dnd::Command> clear_reveal_decoded =
      dnd::DecodeCommand(clear_reveal_bytes.data(), clear_reveal_bytes.size());
  Expect(clear_reveal_decoded.has_value() &&
             std::holds_alternative<dnd::ClearRevealed>(*clear_reveal_decoded),
         "clear reveal command survives serialization");

  const dnd::Command hide_command{
      dnd::HideArea{.cell = CellAt(4, 1), .radius = 2}};
  const std::vector<std::uint8_t> hide_bytes = dnd::EncodeCommand(hide_command);
  const std::optional<dnd::Command> hide_decoded =
      dnd::DecodeCommand(hide_bytes.data(), hide_bytes.size());
  Expect(hide_decoded.has_value() &&
             std::holds_alternative<dnd::HideArea>(*hide_decoded) &&
             std::get<dnd::HideArea>(*hide_decoded).cell == CellAt(4, 1) &&
             std::get<dnd::HideArea>(*hide_decoded).radius == 2,
         "hide area command survives serialization");

  const dnd::Command award_command{
      dnd::AwardXp{.id = 4, .amount = 300, .party = true}};
  const std::vector<std::uint8_t> award_bytes =
      dnd::EncodeCommand(award_command);
  const std::optional<dnd::Command> award_decoded =
      dnd::DecodeCommand(award_bytes.data(), award_bytes.size());
  Expect(award_decoded.has_value() &&
             std::holds_alternative<dnd::AwardXp>(*award_decoded) &&
             std::get<dnd::AwardXp>(*award_decoded).id == 4 &&
             std::get<dnd::AwardXp>(*award_decoded).amount == 300 &&
             std::get<dnd::AwardXp>(*award_decoded).party,
         "xp command survives serialization");

  const dnd::Command set_light{
      dnd::SetTokenLight{.id = 4, .bright_cells = 3, .dim_cells = 6}};
  const std::vector<std::uint8_t> set_light_bytes =
      dnd::EncodeCommand(set_light);
  const std::optional<dnd::Command> set_light_decoded =
      dnd::DecodeCommand(set_light_bytes.data(), set_light_bytes.size());
  Expect(
      set_light_decoded.has_value() &&
          std::holds_alternative<dnd::SetTokenLight>(*set_light_decoded) &&
          std::get<dnd::SetTokenLight>(*set_light_decoded).id == 4 &&
          std::get<dnd::SetTokenLight>(*set_light_decoded).bright_cells == 3 &&
          std::get<dnd::SetTokenLight>(*set_light_decoded).dim_cells == 6,
      "token light command survives serialization");

  const dnd::Event fog_changed{
      dnd::FogChanged{.cells = {dnd::FogCell{.cell = CellAt(1, 1),
                                             .visible = true,
                                             .explored = true,
                                             .light = 1}}}};
  const std::vector<std::uint8_t> fog_changed_bytes =
      dnd::EncodeEvent(fog_changed);
  const std::optional<dnd::Event> fog_changed_decoded =
      dnd::DecodeEvent(fog_changed_bytes.data(), fog_changed_bytes.size());
  Expect(
      fog_changed_decoded.has_value() &&
          std::holds_alternative<dnd::FogChanged>(*fog_changed_decoded) &&
          std::get<dnd::FogChanged>(*fog_changed_decoded).cells.size() == 1 &&
          std::get<dnd::FogChanged>(*fog_changed_decoded).cells.front().light ==
              1,
      "a fog delta carries the light of the cell");

  const dnd::Command attack_command{dnd::AttackToken{
      .attacker = 4, .target = 7, .advantage = true, .reaction = true}};
  const std::vector<std::uint8_t> attack_bytes =
      dnd::EncodeCommand(attack_command);
  const std::optional<dnd::Command> attack_decoded =
      dnd::DecodeCommand(attack_bytes.data(), attack_bytes.size());
  Expect(attack_decoded.has_value() &&
             std::holds_alternative<dnd::AttackToken>(*attack_decoded) &&
             std::get<dnd::AttackToken>(*attack_decoded).attacker == 4 &&
             std::get<dnd::AttackToken>(*attack_decoded).target == 7 &&
             std::get<dnd::AttackToken>(*attack_decoded).advantage &&
             !std::get<dnd::AttackToken>(*attack_decoded).disadvantage &&
             std::get<dnd::AttackToken>(*attack_decoded).reaction,
         "attack command survives serialization");
  const std::vector<std::uint8_t> both_modes{32, 1, 0, 0, 0, 2, 0, 0, 0, 3};
  Expect(!dnd::DecodeCommand(both_modes.data(), both_modes.size()).has_value(),
         "advantage and disadvantage together are rejected by the decoder");

  const dnd::Command turn_action{
      dnd::UseTurnAction{.id = 5, .kind = dnd::TurnActionKind::kDodge}};
  const std::vector<std::uint8_t> turn_action_bytes =
      dnd::EncodeCommand(turn_action);
  const std::optional<dnd::Command> turn_action_decoded =
      dnd::DecodeCommand(turn_action_bytes.data(), turn_action_bytes.size());
  Expect(turn_action_decoded.has_value() &&
             std::holds_alternative<dnd::UseTurnAction>(*turn_action_decoded) &&
             std::get<dnd::UseTurnAction>(*turn_action_decoded).id == 5 &&
             std::get<dnd::UseTurnAction>(*turn_action_decoded).kind ==
                 dnd::TurnActionKind::kDodge,
         "turn action command survives serialization");
  const std::vector<std::uint8_t> unknown_action{35, 5, 0, 0, 0, 8};
  Expect(!dnd::DecodeCommand(unknown_action.data(), unknown_action.size())
              .has_value(),
         "an unknown turn action is rejected");

  const dnd::Event revealed_event{
      dnd::RevealedUpdated{.cells = {CellAt(1, 1), CellAt(2, 2)}}};
  const std::vector<std::uint8_t> revealed_bytes =
      dnd::EncodeEvent(revealed_event);
  const std::optional<dnd::Event> revealed_decoded =
      dnd::DecodeEvent(revealed_bytes.data(), revealed_bytes.size());
  Expect(
      revealed_decoded.has_value() &&
          std::holds_alternative<dnd::RevealedUpdated>(*revealed_decoded) &&
          std::get<dnd::RevealedUpdated>(*revealed_decoded).cells.size() == 2,
      "revealed event survives serialization");

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
      .damage = "1d8+2 дробящий",
      .attack_range = 2,
      .resist = dnd::DamageTypeBit(dnd::DamageType::kCold),
      .vulnerable = dnd::DamageTypeBit(dnd::DamageType::kFire),
      .immune = dnd::DamageTypeBit(dnd::DamageType::kPoison),
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
          std::get<dnd::AddToken>(*add_decoded).damage == "1d8+2 дробящий" &&
          std::get<dnd::AddToken>(*add_decoded).attack_range == 2 &&
          std::get<dnd::AddToken>(*add_decoded).resist ==
              dnd::DamageTypeBit(dnd::DamageType::kCold) &&
          std::get<dnd::AddToken>(*add_decoded).vulnerable ==
              dnd::DamageTypeBit(dnd::DamageType::kFire) &&
          std::get<dnd::AddToken>(*add_decoded).immune ==
              dnd::DamageTypeBit(dnd::DamageType::kPoison),
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

  const dnd::Command sheet_command{dnd::UpdateTokenSheet{
      .id = 4,
      .ac = 15,
      .speed = 9,
      .notes = "Атака: +4, 1d6+2",
      .attack_bonus = 4,
      .save_bonus = 1,
      .initiative_bonus = 3,
      .damage = "1d6+2 огонь",
      .temp_hp = 5,
      .hit_die = 10,
      .con_modifier = 2,
      .attack_range = 6,
      .resist = dnd::DamageTypeBit(dnd::DamageType::kFire),
      .vulnerable = dnd::DamageTypeBit(dnd::DamageType::kCold),
      .immune = dnd::DamageTypeBit(dnd::DamageType::kPoison)}};
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
          std::get<dnd::UpdateTokenSheet>(*sheet_decoded).damage ==
              "1d6+2 огонь" &&
          std::get<dnd::UpdateTokenSheet>(*sheet_decoded).temp_hp == 5 &&
          std::get<dnd::UpdateTokenSheet>(*sheet_decoded).hit_die == 10 &&
          std::get<dnd::UpdateTokenSheet>(*sheet_decoded).con_modifier == 2 &&
          std::get<dnd::UpdateTokenSheet>(*sheet_decoded).attack_range == 6 &&
          std::get<dnd::UpdateTokenSheet>(*sheet_decoded).resist ==
              dnd::DamageTypeBit(dnd::DamageType::kFire) &&
          std::get<dnd::UpdateTokenSheet>(*sheet_decoded).vulnerable ==
              dnd::DamageTypeBit(dnd::DamageType::kCold) &&
          std::get<dnd::UpdateTokenSheet>(*sheet_decoded).immune ==
              dnd::DamageTypeBit(dnd::DamageType::kPoison),
      "sheet command survives serialization");

  const dnd::Command combat_command{dnd::UpdateTokenCombat{
      .id = 4,
      .conditions = {dnd::Condition{"сбит с ног"},
                     dnd::Condition{"отравлен", 60, 15}}}};
  const std::vector<std::uint8_t> combat_bytes =
      dnd::EncodeCommand(combat_command);
  const std::optional<dnd::Command> combat_decoded =
      dnd::DecodeCommand(combat_bytes.data(), combat_bytes.size());
  Expect(
      combat_decoded.has_value() &&
          std::holds_alternative<dnd::UpdateTokenCombat>(*combat_decoded) &&
          std::get<dnd::UpdateTokenCombat>(*combat_decoded).conditions.size() ==
              2 &&
          std::get<dnd::UpdateTokenCombat>(*combat_decoded)
                  .conditions.back()
                  .seconds == 60 &&
          std::get<dnd::UpdateTokenCombat>(*combat_decoded)
                  .conditions.back()
                  .save_dc == 15,
      "combat command with save DC survives serialization");

  const dnd::Command damage{dnd::DamageToken{
      .id = 4, .amount = 7, .damage_type = dnd::DamageType::kFire}};
  const std::vector<std::uint8_t> damage_bytes = dnd::EncodeCommand(damage);
  const std::optional<dnd::Command> damage_decoded =
      dnd::DecodeCommand(damage_bytes.data(), damage_bytes.size());
  Expect(damage_decoded.has_value() &&
             std::holds_alternative<dnd::DamageToken>(*damage_decoded) &&
             std::get<dnd::DamageToken>(*damage_decoded).id == 4 &&
             std::get<dnd::DamageToken>(*damage_decoded).amount == 7 &&
             std::get<dnd::DamageToken>(*damage_decoded).damage_type ==
                 dnd::DamageType::kFire,
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
      dnd::AoeDamage{.expression = "2d6",
                     .save_dc = 15,
                     .damage_type = dnd::DamageType::kFire}};
  const std::vector<std::uint8_t> aoe_damage_bytes =
      dnd::EncodeCommand(aoe_damage);
  const std::optional<dnd::Command> aoe_damage_decoded =
      dnd::DecodeCommand(aoe_damage_bytes.data(), aoe_damage_bytes.size());
  Expect(
      aoe_damage_decoded.has_value() &&
          std::holds_alternative<dnd::AoeDamage>(*aoe_damage_decoded) &&
          std::get<dnd::AoeDamage>(*aoe_damage_decoded).expression == "2d6" &&
          std::get<dnd::AoeDamage>(*aoe_damage_decoded).save_dc == 15 &&
          std::get<dnd::AoeDamage>(*aoe_damage_decoded).damage_type ==
              dnd::DamageType::kFire,
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

  const dnd::Command diced_rest{
      dnd::RestParty{.long_rest = false, .hit_dice = 2}};
  const std::vector<std::uint8_t> diced_rest_bytes =
      dnd::EncodeCommand(diced_rest);
  const std::optional<dnd::Command> diced_rest_decoded =
      dnd::DecodeCommand(diced_rest_bytes.data(), diced_rest_bytes.size());
  Expect(diced_rest_decoded.has_value() &&
             std::holds_alternative<dnd::RestParty>(*diced_rest_decoded) &&
             std::get<dnd::RestParty>(*diced_rest_decoded).hit_dice == 2,
         "the chosen hit dice survive serialization");

  const dnd::Command fall{dnd::FallToken{.id = 7, .feet = 30}};
  const std::vector<std::uint8_t> fall_bytes = dnd::EncodeCommand(fall);
  const std::optional<dnd::Command> fall_decoded =
      dnd::DecodeCommand(fall_bytes.data(), fall_bytes.size());
  Expect(fall_decoded.has_value() &&
             std::holds_alternative<dnd::FallToken>(*fall_decoded) &&
             std::get<dnd::FallToken>(*fall_decoded).id == 7 &&
             std::get<dnd::FallToken>(*fall_decoded).feet == 30,
         "fall command survives serialization");

  const dnd::Command jump{dnd::MoveToken{
      .id = 3, .target = CellAt(2, 2), .crawl = false, .jump = true}};
  const std::vector<std::uint8_t> jump_bytes = dnd::EncodeCommand(jump);
  const std::optional<dnd::Command> jump_decoded =
      dnd::DecodeCommand(jump_bytes.data(), jump_bytes.size());
  Expect(jump_decoded.has_value() &&
             std::holds_alternative<dnd::MoveToken>(*jump_decoded) &&
             std::get<dnd::MoveToken>(*jump_decoded).jump &&
             !std::get<dnd::MoveToken>(*jump_decoded).crawl,
         "the jump flag survives serialization");

  const dnd::Command time{dnd::AdvanceTime{.seconds = 600}};
  const std::vector<std::uint8_t> time_bytes = dnd::EncodeCommand(time);
  const std::optional<dnd::Command> time_decoded =
      dnd::DecodeCommand(time_bytes.data(), time_bytes.size());
  Expect(time_decoded.has_value() &&
             std::holds_alternative<dnd::AdvanceTime>(*time_decoded) &&
             std::get<dnd::AdvanceTime>(*time_decoded).seconds == 600,
         "advance time command survives serialization");

  const dnd::Event updated{dnd::TokenUpdated{
      .token = dnd::Token{.id = 3,
                          .name = "orc",
                          .owner = "dm",
                          .hp = 2,
                          .max_hp = 9,
                          .temp_hp = 3,
                          .hit_die = 12,
                          .con_modifier = -1,
                          .hit_dice_spent = 2,
                          .initiative = 11,
                          .ac = 14,
                          .speed = 12,
                          .notes = "Дубина: +4, 1d8+2",
                          .attack_bonus = 4,
                          .save_bonus = 1,
                          .damage = "1d8+2",
                          .conditions = {dnd::Condition{"оглушён", 2, 13}},
                          .xp = 1200,
                          .movement_used = 5,
                          .death_save_successes = 1,
                          .death_save_failures = 2,
                          .kind = dnd::TokenKind::kMonster,
                          .color = 0x46A864U,
                          .cell = CellAt(1, 1),
                          .action_used = true,
                          .reaction_used = true,
                          .movement_bonus = 6,
                          .disengaged = true,
                          .dodging = true},
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
          std::get<dnd::TokenUpdated>(*updated_decoded).token.xp == 1200 &&
          std::get<dnd::TokenUpdated>(*updated_decoded).token.hit_die == 12 &&
          std::get<dnd::TokenUpdated>(*updated_decoded).token.con_modifier ==
              -1 &&
          std::get<dnd::TokenUpdated>(*updated_decoded).token.hit_dice_spent ==
              2 &&
          std::get<dnd::TokenUpdated>(*updated_decoded)
                  .token.conditions.size() == 1 &&
          std::get<dnd::TokenUpdated>(*updated_decoded)
                  .token.conditions.front()
                  .seconds == 2 &&
          std::get<dnd::TokenUpdated>(*updated_decoded)
                  .token.conditions.front()
                  .save_dc == 13 &&
          std::get<dnd::TokenUpdated>(*updated_decoded).token.movement_used ==
              5 &&
          std::get<dnd::TokenUpdated>(*updated_decoded)
                  .token.death_save_successes == 1 &&
          std::get<dnd::TokenUpdated>(*updated_decoded)
                  .token.death_save_failures == 2 &&
          std::get<dnd::TokenUpdated>(*updated_decoded).token.kind ==
              dnd::TokenKind::kMonster &&
          std::get<dnd::TokenUpdated>(*updated_decoded).token.color ==
              0x46A864U &&
          std::get<dnd::TokenUpdated>(*updated_decoded).token.action_used &&
          !std::get<dnd::TokenUpdated>(*updated_decoded)
               .token.bonus_action_used &&
          std::get<dnd::TokenUpdated>(*updated_decoded).token.reaction_used &&
          std::get<dnd::TokenUpdated>(*updated_decoded).token.movement_bonus ==
              6 &&
          std::get<dnd::TokenUpdated>(*updated_decoded).token.disengaged &&
          std::get<dnd::TokenUpdated>(*updated_decoded).token.dodging,
      "token update survives serialization");

  const dnd::Command start{dnd::StartEncounter{}};
  const std::vector<std::uint8_t> start_bytes = dnd::EncodeCommand(start);
  const std::optional<dnd::Command> start_decoded =
      dnd::DecodeCommand(start_bytes.data(), start_bytes.size());
  Expect(start_decoded.has_value() &&
             std::holds_alternative<dnd::StartEncounter>(*start_decoded),
         "encounter command survives serialization");

  const dnd::Command composition{
      dnd::SetEncounterComposition{.id = 7, .joined = true}};
  const std::vector<std::uint8_t> composition_bytes =
      dnd::EncodeCommand(composition);
  const std::optional<dnd::Command> composition_decoded =
      dnd::DecodeCommand(composition_bytes.data(), composition_bytes.size());
  Expect(
      composition_decoded.has_value() &&
          std::holds_alternative<dnd::SetEncounterComposition>(
              *composition_decoded) &&
          std::get<dnd::SetEncounterComposition>(*composition_decoded).id ==
              7 &&
          std::get<dnd::SetEncounterComposition>(*composition_decoded).joined,
      "encounter composition command survives serialization");

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
}

auto FromHex(std::string_view text) -> std::vector<std::uint8_t> {
  const auto digit = [](char symbol) -> std::uint8_t {
    if (symbol >= '0' && symbol <= '9') {
      return static_cast<std::uint8_t>(symbol - '0');
    }
    if (symbol >= 'a' && symbol <= 'f') {
      return static_cast<std::uint8_t>(symbol - 'a' + 10);
    }
    return 0;
  };
  std::vector<std::uint8_t> bytes;
  bytes.reserve(text.size() / 2);
  for (std::size_t i = 0; i + 1 < text.size(); i += 2) {
    bytes.push_back(
        static_cast<std::uint8_t>((digit(text[i]) << 4) | digit(text[i + 1])));
  }
  return bytes;
}

auto HexBytes(std::span<const std::uint8_t> bytes) -> std::string {
  constexpr std::string_view digits = "0123456789abcdef";
  std::string text;
  text.reserve(bytes.size() * 2);
  for (const std::uint8_t byte : bytes) {
    text.push_back(digits[byte >> 4]);
    text.push_back(digits[byte & 0x0FU]);
  }
  return text;
}

auto ToKey(std::string_view hex) -> dnd::X25519Key {
  dnd::X25519Key key{};
  const std::vector<std::uint8_t> bytes = FromHex(hex);
  std::copy_n(bytes.begin(), std::min(bytes.size(), key.size()), key.begin());
  return key;
}

auto TestX25519() -> void {
  // Векторы RFC 7748, раздел 6.1: по ним видно ошибку в арифметике поля.
  const dnd::X25519Key alice_private =
      ToKey("77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a");
  const dnd::X25519Key bob_private =
      ToKey("5dab087e624a8a4b79e17f8b83800ee66f3bb1292618b6fd1c2f8b27ff88e0eb");

  const dnd::X25519Key alice_public = dnd::X25519PublicKey(alice_private);
  const dnd::X25519Key bob_public = dnd::X25519PublicKey(bob_private);
  Expect(HexBytes(alice_public) ==
             "8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a",
         "x25519 alice public key matches rfc 7748");
  Expect(HexBytes(bob_public) ==
             "de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f",
         "x25519 bob public key matches rfc 7748");

  dnd::X25519Key shared_alice{};
  dnd::X25519Key shared_bob{};
  Expect(dnd::X25519SharedSecret(alice_private, bob_public, shared_alice),
         "x25519 shared secret is valid");
  Expect(dnd::X25519SharedSecret(bob_private, alice_public, shared_bob),
         "x25519 reverse shared secret is valid");
  Expect(shared_alice == shared_bob, "both sides derive the same secret");
  Expect(HexBytes(shared_alice) ==
             "4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742",
         "x25519 shared secret matches rfc 7748");

  // Раздел 5.2: скаляр 9 по базовой точке.
  const dnd::X25519Key scalar =
      ToKey("0900000000000000000000000000000000000000000000000000000000000000");
  Expect(HexBytes(dnd::X25519PublicKey(scalar)) ==
             "422c8e7a6227d7bca1350b3e2bb7279f7897b87bb6854b783c60e80311ae3079",
         "x25519 single iteration matches rfc 7748");

  // Точка малого порядка даёт нулевой секрет: такой обмен ничего не значит.
  dnd::X25519Key weak{};
  Expect(!dnd::X25519SharedSecret(alice_private, dnd::X25519Key{}, weak),
         "x25519 rejects the point of small order");

  const dnd::X25519KeyPair pair = dnd::GenerateX25519KeyPair();
  Expect(pair.public_key == dnd::X25519PublicKey(pair.private_key),
         "generated key pair is consistent");
  Expect(pair.private_key != dnd::X25519Key{},
         "generated private key is not empty");
}

auto TestChaCha20() -> void {
  const std::vector<std::uint8_t> key_bytes = FromHex(
      "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f");
  dnd::ChaCha20Key key{};
  std::ranges::copy(key_bytes, key.begin());

  // Вектор RFC 8439, раздел 2.4.1: блок ключевого потока.
  dnd::ChaCha20Nonce block_nonce{};
  const std::vector<std::uint8_t> nonce_bytes =
      FromHex("000000090000004a00000000");
  std::ranges::copy(nonce_bytes, block_nonce.begin());
  const std::vector<std::uint8_t> block =
      dnd::ChaCha20Xor(key, block_nonce, 1, std::vector<std::uint8_t>(64, 0));
  Expect(HexBytes(block) ==
             "10f1e7e4d13b5915500fdd1fa32071c4c7d1f4c733c068030422aa9ac3d46c4e"
             "d2826446079faa0914c2d705d98b02a2b5129cd1de164eb9cbd083e8a2503c4e",
         "chacha20 block matches rfc 8439");

  // Вектор RFC 8439, раздел 2.3.2: шифрование текста и обратное чтение.
  const std::string plaintext =
      "Ladies and Gentlemen of the class of '99: If I could offer you only "
      "one tip for the future, sunscreen would be it.";
  const std::vector<std::uint8_t> plain(plaintext.begin(), plaintext.end());
  dnd::ChaCha20Nonce text_nonce{};
  const std::vector<std::uint8_t> text_nonce_bytes =
      FromHex("000000000000004a00000000");
  std::ranges::copy(text_nonce_bytes, text_nonce.begin());
  const std::vector<std::uint8_t> ciphertext =
      dnd::ChaCha20Xor(key, text_nonce, 1, plain);
  Expect(HexBytes(ciphertext) ==
             "6e2e359a2568f98041ba0728dd0d6981e97e7aec1d4360c20a27afccfd9fae0b"
             "f91b65c5524733ab8f593dabcd62b3571639d624e65152ab8f530c359f0861d8"
             "07ca0dbf500d6a6156a38e088a22b65e52bc514d16ccf806818ce91ab7793736"
             "5af90bbf74a35be6b40b8eedf2785e42874d",
         "chacha20 encryption matches rfc 8439");
  Expect(dnd::ChaCha20Xor(key, text_nonce, 1, ciphertext) == plain,
         "chacha20 decrypts its own ciphertext");

  // Счётчик блоков: нулевой блок отличается от первого.
  const std::vector<std::uint8_t> first_bytes =
      dnd::ChaCha20Xor(key, text_nonce, 0, std::vector<std::uint8_t>(16, 0));
  Expect(HexBytes(first_bytes) == "af051e40bba0354981329a806a140eaf",
         "chacha20 starts at the requested block");
}

auto TestHkdf() -> void {
  // Вектор RFC 5869, тест 1.
  const std::vector<std::uint8_t> ikm(22, 0x0b);
  const std::vector<std::uint8_t> salt = FromHex("000102030405060708090a0b0c");
  const std::vector<std::uint8_t> info = FromHex("f0f1f2f3f4f5f6f7f8f9");
  const std::vector<std::uint8_t> okm = dnd::HkdfSha256(ikm, salt, info, 42);
  Expect(HexBytes(okm) ==
             "3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf"
             "34007208d5b887185865",
         "hkdf-sha256 matches rfc 5869");
  Expect(dnd::HkdfSha256(ikm, salt, info, 32) ==
             std::vector<std::uint8_t>(okm.begin(), okm.begin() + 32),
         "hkdf output starts the same for shorter requests");
}

auto TestHandshakeProofs() -> void {
  const dnd::ChallengeNonce nonce{};
  const dnd::X25519Key host_key =
      ToKey("8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a");
  const dnd::X25519Key client_key =
      ToKey("de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f");
  const std::vector<std::uint8_t> transcript =
      dnd::HandshakeTranscript(nonce, host_key, client_key, "player");

  const dnd::PasswordProof proof = dnd::MakeClientProof("secret", transcript);
  Expect(dnd::ProofMatches(proof, dnd::MakeClientProof("secret", transcript)),
         "proof is deterministic for the same transcript");
  Expect(!dnd::ProofMatches(proof, dnd::MakeClientProof("other", transcript)),
         "proof changes with the password");

  // Подмена любого элемента транскрипта меняет отпечаток: иначе можно было
  // бы подменить ключи или имя и остаться с тем же доказательством.
  const std::vector<std::uint8_t> other_nonce = dnd::HandshakeTranscript(
      dnd::ChallengeNonce{1}, host_key, client_key, "player");
  const std::vector<std::uint8_t> other_host =
      dnd::HandshakeTranscript(nonce, client_key, client_key, "player");
  const std::vector<std::uint8_t> other_name =
      dnd::HandshakeTranscript(nonce, host_key, client_key, "player2");
  Expect(!dnd::ProofMatches(proof, dnd::MakeClientProof("secret", other_nonce)),
         "proof changes with the challenge");
  Expect(!dnd::ProofMatches(proof, dnd::MakeClientProof("secret", other_host)),
         "proof changes with the host key");
  Expect(!dnd::ProofMatches(proof, dnd::MakeClientProof("secret", other_name)),
         "proof changes with the player name");

  Expect(!dnd::ProofMatches(proof, dnd::MakeServerProof("secret", transcript)),
         "server proof differs from the client proof");
}

auto TestSessionCipher() -> void {
  const dnd::X25519Key shared =
      ToKey("4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742");
  const dnd::ChallengeNonce nonce = dnd::RandomChallenge();
  const dnd::SessionSecrets secrets = dnd::DeriveSessionSecrets(shared, nonce);
  dnd::SessionCipher host = dnd::SessionCipher::ForHost(secrets);
  dnd::SessionCipher client = dnd::SessionCipher::ForClient(secrets);

  const std::string text = "ход: аллея, 3 клетки";
  const std::vector<std::uint8_t> payload(text.begin(), text.end());
  const std::vector<std::uint8_t> sealed = client.Seal(payload);
  Expect(sealed.size() == payload.size() + dnd::kSealedFrameOverhead,
         "sealed frame carries the marker, number, mac and ciphertext");
  Expect(sealed[0] == dnd::kSealedFrameMarker, "sealed frame is marked");
  Expect(std::ranges::search(sealed, payload).empty(),
         "payload is not visible in the sealed frame");

  std::vector<std::uint8_t> opened;
  Expect(host.Open(sealed, opened) && opened == payload,
         "host opens the sealed frame");

  // Повтор того же кадра отвергается: номер кадра уже израсходован.
  std::vector<std::uint8_t> replayed;
  Expect(!host.Open(sealed, replayed), "replayed frame is rejected");

  // Битая полезная часть и битый MAC тоже не проходят. Кадры портят
  // отдельную пару, чтобы не сбить порядок номеров у основной.
  dnd::SessionCipher host_other = dnd::SessionCipher::ForHost(secrets);
  dnd::SessionCipher client_other = dnd::SessionCipher::ForClient(secrets);
  std::vector<std::uint8_t> tampered = client_other.Seal(payload);
  tampered[1 + 8 + 1] ^= 0x01;
  Expect(!host_other.Open(tampered, replayed),
         "tampered ciphertext is rejected");

  std::vector<std::uint8_t> broken_mac = client_other.Seal(payload);
  broken_mac.back() ^= 0x01;
  Expect(!host_other.Open(broken_mac, replayed), "tampered mac is rejected");

  // Чужое направление: клиент не читает собственные кадры.
  dnd::SessionCipher client_again = dnd::SessionCipher::ForClient(secrets);
  std::vector<std::uint8_t> own;
  Expect(!client_again.Open(client_again.Seal(payload), own),
         "frames of the wrong direction are rejected");

  // Порядок кадров соблюдается: следующий честный кадр после отказов читается.
  const std::vector<std::uint8_t> next = client.Seal(payload);
  Expect(host.Open(next, opened) && opened == payload,
         "sequence continues after rejected frames");
}

auto Near(float first, float second) -> bool {
  return std::fabs(first - second) < 0.001F;
}

auto TestCamera() -> void {
  const dnd::Camera plain{};
  Expect(plain.cell_size == dnd::kDefaultCellSize, "camera starts at 48 px");
  Expect(dnd::CameraCellAt(plain, 0.0F, 0.0F) == CellAt(0, 0),
         "top-left pixel is the first cell");
  Expect(dnd::CameraCellAt(plain, 47.9F, 47.9F) == CellAt(0, 0),
         "the whole first cell maps to zero");
  Expect(dnd::CameraCellAt(plain, 48.0F, 48.0F) == CellAt(1, 1),
         "a cell boundary starts the next cell");
  Expect(dnd::CameraCellAt(plain, -1.0F, -1.0F) == CellAt(-1, -1),
         "pixels left of the map give negative cells");

  dnd::Camera shifted{};
  shifted.x = -96.0F;
  shifted.y = -48.0F;
  Expect(dnd::CameraCellAt(shifted, 0.0F, 0.0F) == CellAt(2, 1),
         "camera offset shifts cell coordinates");
  Expect(dnd::CameraCellAt(shifted, 95.0F, 47.0F) == CellAt(3, 1),
         "floor keeps the point inside the cell under it");

  const dnd::Camera centered = dnd::CenteredCamera(1280.0F, 720.0F, 24, 12);
  Expect(Near(centered.x, 64.0F) && Near(centered.y, 72.0F),
         "a 24x12 map is centered in a 1280x720 window");
  const dnd::Camera small = dnd::CenteredCamera(100.0F, 100.0F, 24, 12, 24.0F);
  Expect(Near(small.x, -238.0F) && Near(small.y, -94.0F),
         "a map larger than the window starts off-screen and centered");

  dnd::Camera panned{};
  dnd::PanCamera(panned, 5.0F, -3.0F);
  dnd::PanCamera(panned, 2.0F, -4.0F);
  Expect(Near(panned.x, 7.0F) && Near(panned.y, -7.0F),
         "panning adds the mouse delta");

  dnd::Camera zoom{};
  zoom.cell_size = 50.0F;
  zoom.x = 10.0F;
  zoom.y = 20.0F;
  dnd::ZoomCamera(zoom, 100.0F, 100.0F, 2.0F);
  Expect(Near(zoom.cell_size, 58.0F), "wheel changes the cell size by 4 px");
  Expect(Near(zoom.x, 100.0F - (90.0F / 50.0F * 58.0F)),
         "zoom keeps the world point under the cursor on x");
  Expect(Near(zoom.y, 100.0F - (80.0F / 50.0F * 58.0F)),
         "zoom keeps the world point under the cursor on y");

  dnd::Camera clamped{};
  clamped.cell_size = 90.0F;
  dnd::ZoomCamera(clamped, 0.0F, 0.0F, 10.0F);
  Expect(Near(clamped.cell_size, dnd::kMaxCellSize), "zoom stops at 96 px");
  dnd::ZoomCamera(clamped, 0.0F, 0.0F, -100.0F);
  Expect(Near(clamped.cell_size, dnd::kMinCellSize), "zoom stops at 24 px");

  const dnd::Camera untouched = clamped;
  dnd::ZoomCamera(clamped, 10.0F, 10.0F, 0.0F);
  Expect(Near(clamped.cell_size, untouched.cell_size) &&
             Near(clamped.x, untouched.x) && Near(clamped.y, untouched.y),
         "zero wheel leaves the camera untouched");

  const dnd::Camera on_cell =
      dnd::CameraOnCell(1280.0F, 720.0F, 100, 100, CellAt(80, 90));
  Expect(Near(on_cell.x, 640.0F - (80.5F * 48.0F)) &&
             Near(on_cell.y, 360.0F - (90.5F * 48.0F)),
         "camera centers the focus cell on a large map");
  Expect(dnd::CameraCellAt(on_cell, 640.0F, 360.0F) == CellAt(80, 90),
         "the focus cell is under the window center");
  const dnd::Camera on_edge =
      dnd::CameraOnCell(1280.0F, 720.0F, 100, 100, CellAt(0, 0));
  Expect(Near(on_edge.x, 0.0F) && Near(on_edge.y, 0.0F),
         "a focus cell at the map edge does not scroll the map away");
  const dnd::Camera small_on_cell =
      dnd::CameraOnCell(1280.0F, 720.0F, 10, 10, CellAt(1, 1));
  Expect(Near(small_on_cell.x, 400.0F) && Near(small_on_cell.y, 120.0F),
         "a small map stays centered whatever the focus cell");

  const dnd::FogOfWar no_fog(4, 4);
  const std::vector<dnd::Token> focus_tokens = {
      dnd::Token{.id = 1,
                 .owner = "Bob",
                 .kind = dnd::TokenKind::kPlayer,
                 .cell = CellAt(2, 2)},
      dnd::Token{.id = 2,
                 .owner = "",
                 .kind = dnd::TokenKind::kPlayer,
                 .cell = CellAt(7, 8)},
      dnd::Token{.id = 3,
                 .owner = "dm",
                 .kind = dnd::TokenKind::kMonster,
                 .cell = CellAt(9, 9)},
  };
  Expect(dnd::InitialFocusCell(focus_tokens, no_fog, "ann") == CellAt(7, 8),
         "without a character the first claimable one is shown");
  const std::vector<dnd::Token> own_tokens = {
      focus_tokens[1],
      dnd::Token{.id = 4,
                 .owner = "Ann",
                 .kind = dnd::TokenKind::kPlayer,
                 .cell = CellAt(5, 6)},
  };
  Expect(dnd::InitialFocusCell(own_tokens, no_fog, "ann") == CellAt(5, 6),
         "the player's own character wins over a claimable one");
  Expect(
      dnd::InitialFocusCell({focus_tokens[0]}, no_fog, "ann") == CellAt(2, 2),
      "a party character is shown when nothing is claimable");
  Expect(
      dnd::InitialFocusCell({focus_tokens[2]}, no_fog, "ann") == CellAt(9, 9),
      "a visible monster is better than an empty map");
  Expect(!dnd::InitialFocusCell({}, no_fog, "ann").has_value(),
         "no tokens and no explored cells leave the camera centered");
  dnd::FogOfWar explored(4, 4);
  explored.SetCell(CellAt(1, 1), true, true);
  explored.SetCell(CellAt(2, 1), true, true);
  Expect(dnd::InitialFocusCell({}, explored, "ann") == CellAt(2, 1),
         "without tokens the explored area is shown");
}

auto TestAutosave() -> void {
  using Clock = dnd::AutosaveTimer::Clock;
  const Clock::time_point start{};
  dnd::AutosaveTimer timer;
  Expect(!timer.Dirty(), "autosave starts clean");
  Expect(!timer.Due(start + std::chrono::hours(1)),
         "a clean timer is never due");

  timer.MarkChanged(start);
  Expect(timer.Dirty(), "a change marks the campaign dirty");
  Expect(!timer.Due(start + std::chrono::seconds{7}),
         "autosave waits for the full delay");
  timer.MarkChanged(start + std::chrono::seconds{5});
  Expect(!timer.Due(start + std::chrono::seconds{7}),
         "later changes do not move the first deadline");
  Expect(timer.Due(start + dnd::kAutosaveDelay),
         "autosave fires eight seconds after the first change");

  timer.SaveFailed(start + std::chrono::seconds{10});
  Expect(timer.Dirty() && !timer.Due(start + std::chrono::seconds{17}) &&
             timer.Due(start + std::chrono::seconds{18}),
         "a failed write retries after another full delay");

  timer.Saved();
  Expect(!timer.Dirty() && !timer.Due(start + std::chrono::seconds{100}),
         "a successful write clears the timer");

  timer.MarkChanged(start + std::chrono::seconds{20});
  timer.ChangedDuringSave(start + std::chrono::seconds{25});
  Expect(timer.Dirty() && !timer.Due(start + std::chrono::seconds{32}) &&
             timer.Due(start + std::chrono::seconds{33}),
         "a change during the write schedules another pass");
}

auto TestLogScroll() -> void {
  dnd::LogScroll scroll(5);
  Expect(scroll.Offset() == 0, "log starts at the newest line");
  scroll.Scroll(1, 10);
  Expect(scroll.Offset() == 1, "PageUp moves one line up until the page");
  scroll.Scroll(100, 10);
  Expect(scroll.Offset() == 5, "scrolling up stops at total minus page");
  scroll.Scroll(-2, 10);
  Expect(scroll.Offset() == 3, "scrolling down reduces the offset");

  scroll.OnAppend(12);
  Expect(scroll.Offset() == 5, "new lines do not shift the lines being read");
  scroll.OnAppend(20);
  Expect(scroll.Offset() == 13, "several new lines shift by the amount grown");
  scroll.ToNewest();
  Expect(scroll.Offset() == 0, "End returns to the newest line");
  scroll.OnAppend(25);
  Expect(scroll.Offset() == 0, "sticking to the bottom survives new lines");

  scroll.Scroll(100, 25);
  Expect(scroll.Offset() == 20, "a long log scrolls by its full history");
  scroll.OnAppend(19);
  Expect(scroll.Offset() == 14, "shrinking the log clamps the offset");
  scroll.Scroll(3, 2);
  Expect(scroll.Offset() == 0, "a log shorter than the page cannot scroll");

  dnd::LogScroll window_scroll(3);
  window_scroll.Scroll(100, 10);
  Expect(window_scroll.Offset() == 7, "a small page scrolls to the first line");
  window_scroll.SetVisibleLines(8);
  Expect(window_scroll.Offset() == 2,
         "a bigger page clamps the scroll position");
}

auto TestDeleteConfirm() -> void {
  using Clock = dnd::DeleteConfirm::Clock;
  const Clock::time_point start{};
  dnd::DeleteConfirm confirm;
  Expect(!confirm.Request(7, start), "the first Del only warns");
  Expect(confirm.Pending() == 7, "the warned object is remembered");
  Expect(confirm.Request(7, start + std::chrono::seconds{1}),
         "a repeat within three seconds confirms");
  Expect(confirm.Pending() == dnd::kNoConfirmId,
         "a confirmed request is cleared");
  Expect(!confirm.Request(7, start + std::chrono::seconds{2}),
         "after a delete the next warning starts over");

  dnd::DeleteConfirm other;
  Expect(!other.Request(8, start), "another object starts its own warning");
  Expect(!other.Request(9, start + std::chrono::seconds{1}),
         "a different object does not confirm the first");
  Expect(!other.Request(8, start + std::chrono::seconds{5}),
         "a repeat after the delay only warns again");
  Expect(other.Request(8, start + std::chrono::seconds{6}),
         "a fresh repeat confirms");

  dnd::DeleteConfirm deadline;
  Expect(!deadline.Request(1, start),
         "the deadline check starts with a warning");
  Expect(deadline.Request(1, start + dnd::kDeleteConfirmDelay),
         "the repeat exactly on the deadline still confirms");
}

auto TestStatusMessage() -> void {
  using Clock = dnd::StatusMessage::Clock;
  const Clock::time_point start{};
  dnd::StatusMessage status;
  Expect(status.Empty(), "a status starts empty");

  status.Set("нельзя удалить последнюю карту", start);
  Expect(status.Text() == "нельзя удалить последнюю карту",
         "a status keeps its text");
  status.Expire(start + std::chrono::seconds{2});
  Expect(!status.Empty(), "the status stays for three seconds");
  status.Expire(start + dnd::kStatusMessageTtl);
  Expect(status.Empty(), "the status expires on the deadline");
  Expect(status.Text().empty(), "an expired status is gone");

  status.Set("first", start);
  status.Set("second", start + std::chrono::seconds{1});
  status.Expire(start + dnd::kStatusMessageTtl);
  Expect(status.Text() == "second", "a new text restarts the deadline");
  status.Expire(start + std::chrono::seconds{4});
  Expect(status.Empty(), "the restarted deadline expires");

  status.Set("hidden", start);
  status.Clear();
  Expect(status.Empty(), "clear hides the status at once");
}

auto TestMenuModel() -> void {
  const std::span<const dnd::MenuField> host =
      dnd::MenuFieldOrder(dnd::MenuTab::kHost);
  Expect(host.size() == 2 && host[0] == dnd::MenuField::kPort &&
             host[1] == dnd::MenuField::kPassword,
         "the host tab edits port and password");
  const std::span<const dnd::MenuField> join =
      dnd::MenuFieldOrder(dnd::MenuTab::kJoin);
  Expect(join.size() == 4 && join[0] == dnd::MenuField::kAddress &&
             join[1] == dnd::MenuField::kPort &&
             join[2] == dnd::MenuField::kName &&
             join[3] == dnd::MenuField::kPassword,
         "the join tab edits address, port, name and password");
  Expect(dnd::MenuFieldOrder(dnd::MenuTab::kEditor).empty(),
         "the editor tab has no text fields");

  Expect(dnd::NextMenuField(dnd::MenuTab::kHost, dnd::MenuField::kPort, 1) ==
             dnd::MenuField::kPassword,
         "Tab moves to the next host field");
  Expect(dnd::NextMenuField(dnd::MenuTab::kHost, dnd::MenuField::kPassword,
                            1) == dnd::MenuField::kPort,
         "Tab wraps around the host fields");
  Expect(dnd::NextMenuField(dnd::MenuTab::kHost, dnd::MenuField::kPassword,
                            -1) == dnd::MenuField::kPort,
         "Shift+Tab moves backwards");
  Expect(dnd::NextMenuField(dnd::MenuTab::kHost, dnd::MenuField::kNone, -1) ==
             dnd::MenuField::kPassword,
         "Shift+Tab from no field selects the last field");
  Expect(dnd::NextMenuField(dnd::MenuTab::kEditor, dnd::MenuField::kNone, 1) ==
             dnd::MenuField::kNone,
         "the editor tab never focuses a field");

  Expect(dnd::NextMenuTab(dnd::MenuTab::kHost, -1) == dnd::MenuTab::kEditor,
         "left from the host tab wraps to the editor");
  Expect(dnd::NextMenuTab(dnd::MenuTab::kEditor, 1) == dnd::MenuTab::kHost,
         "right from the editor wraps to the host");
  Expect(dnd::NextMenuTab(dnd::MenuTab::kJoin, -1) == dnd::MenuTab::kHost,
         "left from the join tab goes to the host");

  Expect(dnd::MenuFieldLimit(dnd::MenuField::kPort) == 5,
         "the port field holds five bytes");
  Expect(dnd::MenuFieldLimit(dnd::MenuField::kName) == 64,
         "the name field holds 64 bytes");
  Expect(dnd::MenuFieldLimit(dnd::MenuField::kNone) == 0,
         "no field accepts no text");

  dnd::MenuState menu;
  menu.active_field = dnd::MenuField::kPort;
  Expect(dnd::ActiveMenuText(menu) == &menu.port,
         "the active field points at the port buffer");
  menu.tab = dnd::MenuTab::kJoin;
  menu.active_field = dnd::MenuField::kAddress;
  Expect(dnd::ActiveMenuText(menu) == &menu.address,
         "the join tab edits the address");
  menu.tab = dnd::MenuTab::kHost;
  Expect(dnd::ActiveMenuText(menu) == nullptr,
         "the host tab has no address field");
  menu.tab = dnd::MenuTab::kEditor;
  menu.active_field = dnd::MenuField::kPort;
  Expect(dnd::ActiveMenuText(menu) == nullptr,
         "the editor tab does not edit text");

  std::uint16_t port = 0;
  Expect(dnd::ParsePort("7777", port) && port == 7777, "a plain port parses");
  Expect(dnd::ParsePort("65535", port) && port == 65535,
         "the largest port parses");
  Expect(!dnd::ParsePort("0", port), "port zero is rejected");
  Expect(!dnd::ParsePort("65536", port), "ports above 65535 are rejected");
  Expect(!dnd::ParsePort("", port), "an empty port is rejected");
  Expect(!dnd::ParsePort("-1", port), "a negative port is rejected");
  Expect(!dnd::ParsePort(" 80", port), "leading spaces are rejected");
  Expect(!dnd::ParsePort("80 ", port), "trailing spaces are rejected");
  Expect(!dnd::ParsePort("80a", port), "trailing letters are rejected");

  const dnd::HostAddress named = dnd::SplitHostPort("example.org:7778");
  Expect(named.host == "example.org" && named.has_port && named.port == 7778,
         "host:port splits into host and port");
  const dnd::HostAddress plain_host = dnd::SplitHostPort("example.org");
  Expect(plain_host.host == "example.org" && !plain_host.has_port,
         "a host without a port stays whole");
  const dnd::HostAddress bad_port = dnd::SplitHostPort("example.org:0");
  Expect(bad_port.host == "example.org:0" && !bad_port.has_port,
         "an invalid port keeps the whole address");
  const dnd::HostAddress empty_host = dnd::SplitHostPort(":7777");
  Expect(
      empty_host.host.empty() && empty_host.has_port && empty_host.port == 7777,
      "an empty host with a port is reported as such");

  // Строка партии: нет файла, новая партия, продолжение или чужая версия.
  const dnd::GameSnapshot plain{.map = dnd::GridMap(3, 3), .next_token_id = 1};
  const dnd::Campaign campaign = dnd::MakeCampaign(plain, "Шаблон");
  dnd::Overlay overlay = dnd::FreshOverlay(campaign);
  dnd::PartySummary summary = dnd::SummarizeParty(overlay, false, false);
  Expect(summary.state == dnd::PartyState::kNone,
         "a missing party file is reported as none");
  summary = dnd::SummarizeParty(overlay, true, false);
  Expect(summary.state == dnd::PartyState::kNew,
         "an empty party file is a new party");
  Expect(dnd::SetSceneSnapshot(overlay, campaign.scenes.front().id, plain) &&
             dnd::SetSceneName(overlay, 7, "Имя"),
         "the party has changes to count");
  summary = dnd::SummarizeParty(overlay, true, false);
  Expect(summary.state == dnd::PartyState::kContinued &&
             summary.changed_scenes == 2 && summary.played_scenes == 1,
         "a continued party counts changed and played scenes");
  summary = dnd::SummarizeParty(overlay, true, true);
  Expect(summary.state == dnd::PartyState::kMismatch,
         "a party from another campaign needs confirmation");

  Expect(dnd::MenuCommandForChar(dnd::MenuTab::kHost, 'n') ==
                 dnd::MenuCommand::kNewParty &&
             dnd::MenuCommandForChar(dnd::MenuTab::kHost, 'R') ==
                 dnd::MenuCommand::kResetScene &&
             dnd::MenuCommandForChar(dnd::MenuTab::kHost, 'a') ==
                 dnd::MenuCommand::kAcceptParty,
         "the host tab maps party keys");
  Expect(dnd::MenuCommandForChar(dnd::MenuTab::kJoin, 'n') ==
             dnd::MenuCommand::kNone,
         "other tabs do not start a new party");
}

auto TestBottomHints() -> void {
  const auto has = [](const std::vector<dnd::Hint>& hints,
                      dnd::Hint hint) -> bool {
    return std::ranges::find(hints, hint) != hints.end();
  };

  // Игрок без выделения: общие клавиши и приглашение выбрать токен, без
  // клавиш токена и без добавок DM.
  const dnd::HintContext player;
  const std::vector<dnd::Hint> player_hints = dnd::BottomHints(player);
  Expect(!player_hints.empty() && player_hints[0] == dnd::Hint::kChat,
         "the bottom hint starts with chat");
  Expect(has(player_hints, dnd::Hint::kJournal),
         "the journal is offered to every role");
  Expect(has(player_hints, dnd::Hint::kExitMenu),
         "every role can return to the menu");
  Expect(has(player_hints, dnd::Hint::kSelect),
         "no selection asks to pick a token");
  Expect(!has(player_hints, dnd::Hint::kAttack) &&
             !has(player_hints, dnd::Hint::kSheet),
         "no selection hides token keys");
  Expect(!has(player_hints, dnd::Hint::kDmSpawn) &&
             !has(player_hints, dnd::Hint::kScenes),
         "a player gets no DM hints");

  // Свой персонаж: клавиши токена и лист; цвет меняет только DM.
  dnd::HintContext own;
  own.has_selection = true;
  own.can_view_sheet = true;
  const std::vector<dnd::Hint> own_hints = dnd::BottomHints(own);
  Expect(!has(own_hints, dnd::Hint::kSelect),
         "a selection hides the pick hint");
  Expect(has(own_hints, dnd::Hint::kAttack) &&
             has(own_hints, dnd::Hint::kSave) &&
             has(own_hints, dnd::Hint::kSheet) &&
             has(own_hints, dnd::Hint::kConditions),
         "a selection shows token keys");
  Expect(has(own_hints, dnd::Hint::kJump) && !has(own_hints, dnd::Hint::kCrawl),
         "a standing token offers jump but not crawl");
  own.prone = true;
  Expect(has(dnd::BottomHints(own), dnd::Hint::kCrawl),
         "a prone token offers crawling");
  Expect(
      !has(own_hints, dnd::Hint::kClaim) && !has(own_hints, dnd::Hint::kColor),
      "an owned character offers neither claim nor colour");

  // Чужой персонаж: состояния и лист на просмотр, цвет — нет.
  dnd::HintContext foreign;
  foreign.has_selection = true;
  foreign.can_view_sheet = true;
  const std::vector<dnd::Hint> foreign_hints = dnd::BottomHints(foreign);
  Expect(has(foreign_hints, dnd::Hint::kAttack) &&
             has(foreign_hints, dnd::Hint::kMoveRange),
         "a foreign token still shows attack and reach");
  Expect(has(foreign_hints, dnd::Hint::kConditions),
         "a foreign token still shows conditions");
  Expect(has(foreign_hints, dnd::Hint::kSheet),
         "a foreign character sheet opens for viewing");
  Expect(!has(foreign_hints, dnd::Hint::kColor) &&
             !has(foreign_hints, dnd::Hint::kClaim),
         "a foreign character hides colour and claim");

  // Свободный персонаж игроку без своего: забрать.
  dnd::HintContext claimable;
  claimable.has_selection = true;
  claimable.can_claim = true;
  const std::vector<dnd::Hint> claimable_hints = dnd::BottomHints(claimable);
  Expect(has(claimable_hints, dnd::Hint::kClaim),
         "a free character offers claim");

  // DM: добавки роли, а ход — только в бою.
  dnd::HintContext dm;
  dm.is_dm = true;
  dm.has_selection = true;
  std::vector<dnd::Hint> dm_hints = dnd::BottomHints(dm);
  Expect(has(dm_hints, dnd::Hint::kDmSpawn) &&
             has(dm_hints, dnd::Hint::kBestiary) &&
             has(dm_hints, dnd::Hint::kScenes) &&
             has(dm_hints, dnd::Hint::kDmUndo),
         "the DM gets spawn, bestiary, scenes and undo");
  Expect(has(dm_hints, dnd::Hint::kColor), "the DM can recolour tokens");
  Expect(!has(dm_hints, dnd::Hint::kDmTurn),
         "out of combat the DM has no turn key");
  dm.in_combat = true;
  dm_hints = dnd::BottomHints(dm);
  Expect(has(dm_hints, dnd::Hint::kDmTurn), "in combat the DM advances turns");

  // Прокрученный лог напоминает про End.
  dnd::HintContext scrolled;
  scrolled.log_scrolled = true;
  Expect(has(dnd::BottomHints(scrolled), dnd::Hint::kLogNewest),
         "a scrolled log hints at End");

  // Открытый режим показывает только свои клавиши.
  dnd::HintContext aoe;
  aoe.mode = dnd::HintMode::kAoe;
  aoe.is_dm = true;
  const std::vector<dnd::Hint> aoe_hints = dnd::BottomHints(aoe);
  Expect(aoe_hints.size() == 1 && aoe_hints[0] == dnd::Hint::kAoeKeys,
         "the area template shows only its own keys");
  dnd::HintContext sheet;
  sheet.mode = dnd::HintMode::kSheet;
  Expect(dnd::BottomHints(sheet) ==
             std::vector<dnd::Hint>{dnd::Hint::kSheetViewKeys},
         "a player's sheet shows only view keys");
  sheet.is_dm = true;
  Expect(
      dnd::BottomHints(sheet) == std::vector<dnd::Hint>{dnd::Hint::kSheetKeys},
      "the DM's sheet shows editing keys");
  dnd::HintContext conditions;
  conditions.mode = dnd::HintMode::kConditions;
  Expect(dnd::BottomHints(conditions) ==
             std::vector<dnd::Hint>{dnd::Hint::kConditionsViewKeys},
         "a player's conditions show only view keys");
  conditions.is_dm = true;
  Expect(dnd::BottomHints(conditions) ==
             std::vector<dnd::Hint>{dnd::Hint::kConditionsKeys},
         "the DM's conditions show editing keys");
  dnd::HintContext scenes;
  scenes.mode = dnd::HintMode::kScenes;
  Expect(dnd::BottomHints(scenes) ==
             std::vector<dnd::Hint>{dnd::Hint::kScenesKeys},
         "the scene list shows only its own keys");
}

auto TestNavigation() -> void {
  Expect(dnd::WrapIndex(0, -1, 3) == 2, "wrapping left from the first item");
  Expect(dnd::WrapIndex(2, 1, 3) == 0, "wrapping right from the last item");
  Expect(dnd::WrapIndex(1, 3, 5) == 4, "wrapping skips whole laps");
  Expect(dnd::WrapIndex(4, 3, 5) == 2, "wrapping handles a negative result");
  Expect(dnd::WrapIndex(2, 1, 0) == 0, "an empty list wraps to zero");

  Expect(dnd::StepIndex(0, -1, 3) == 0, "the first item cannot step up");
  Expect(dnd::StepIndex(2, 1, 3) == 2, "the last item cannot step down");
  Expect(dnd::StepIndex(1, -1, 3) == 0, "stepping up moves one item");
  Expect(dnd::StepIndex(1, 1, 3) == 2, "stepping down moves one item");
  Expect(dnd::StepIndex(1, 0, 3) == 1, "a zero step keeps the index");
  Expect(dnd::StepIndex(5, 1, 3) == 2, "an out-of-range index is clamped");
  Expect(dnd::StepIndex(4, -1, 0) == 0, "an empty list steps to zero");

  Expect(dnd::IndexAfterDelete(0, 2) == 0,
         "deleting the first item opens the new first");
  Expect(dnd::IndexAfterDelete(1, 2) == 0,
         "deleting a middle item opens the previous one");
  Expect(dnd::IndexAfterDelete(2, 2) == 1,
         "deleting the last item opens the previous one");
  Expect(dnd::IndexAfterDelete(5, 2) == 1,
         "a stale removed index still opens the last remaining item");
  Expect(dnd::IndexAfterDelete(0, 0) == 0,
         "deleting the only item leaves index zero");

  Expect(dnd::IndexForDigit(1, 3) == 0, "digit 1 opens the first scene");
  Expect(dnd::IndexForDigit(3, 3) == 2, "digit 3 opens the third scene");
  Expect(!dnd::IndexForDigit(4, 3).has_value(),
         "a digit without a scene opens nothing");
  Expect(!dnd::IndexForDigit(0, 3).has_value(), "digit zero opens nothing");
  Expect(dnd::IndexForDigit(9, 10) == 8,
         "digit 9 reaches the ninth scene in a longer campaign");
}

auto TestEditorPaint() -> void {
  dnd::PaintDrag drag;
  Expect(!drag.Active(), "a paint stroke starts idle");
  const std::vector<dnd::Cell> idle = drag.Move(dnd::Cell{.x = 4, .y = 2});
  Expect(idle.empty() && !drag.Active(), "a move without Begin paints nothing");

  drag.Begin(dnd::Cell{.x = 0, .y = 0});
  Expect(drag.Move(dnd::Cell{.x = 0, .y = 0}).empty(),
         "the same cell paints nothing");
  const std::vector<dnd::Cell> diagonal = drag.Move(dnd::Cell{.x = 3, .y = 3});
  Expect(diagonal.size() == 4, "a diagonal stroke walks every cell");
  Expect(diagonal.front() == dnd::Cell{.x = 0, .y = 0} &&
             diagonal.back() == dnd::Cell{.x = 3, .y = 3},
         "the diagonal stroke includes both ends");
  Expect(diagonal[1] == dnd::Cell{.x = 1, .y = 1},
         "the diagonal stroke has no holes");

  const std::vector<dnd::Cell> jump = drag.Move(dnd::Cell{.x = 10, .y = 0});
  Expect(jump.size() == 8, "a far move paints every cell of the line");
  Expect(jump.front() == dnd::Cell{.x = 3, .y = 3} &&
             jump.back() == dnd::Cell{.x = 10, .y = 0},
         "the jump starts at the previous cell");
  Expect(jump[7] == dnd::Cell{.x = 10, .y = 0},
         "the jump ends exactly on the cursor cell");

  drag.End();
  Expect(!drag.Active(), "End finishes the stroke");
  Expect(drag.Move(dnd::Cell{.x = 0, .y = 0}).empty(),
         "a move right after End paints nothing");

  const std::vector<dnd::Cell> line =
      dnd::LineCells(dnd::Cell{.x = 5, .y = 5}, dnd::Cell{.x = 5, .y = 5});
  Expect(line.size() == 1 && line.front() == dnd::Cell{.x = 5, .y = 5},
         "a zero-length line is one cell");
}

auto TestEditorTokenMove() -> void {
  dnd::GridMap map(4, 4);
  std::vector<dnd::Token> tokens;
  dnd::Token first;
  first.id = 1;
  first.name = "Первый";
  first.cell = dnd::Cell{.x = 1, .y = 1};
  tokens.push_back(first);
  dnd::Token second;
  second.id = 2;
  second.name = "Второй";
  second.cell = dnd::Cell{.x = 2, .y = 1};
  tokens.push_back(second);

  Expect(dnd::CanPlaceToken(map, tokens, dnd::Cell{.x = 0, .y = 0}, 1),
         "a free floor cell accepts a token");
  Expect(!dnd::CanPlaceToken(map, tokens, dnd::Cell{.x = 2, .y = 1}, 1),
         "a cell with another token is occupied");
  Expect(dnd::CanPlaceToken(map, tokens, dnd::Cell{.x = 2, .y = 1}, 2),
         "the moving token does not block its own cell");
  Expect(!dnd::CanPlaceToken(map, tokens, dnd::Cell{.x = 4, .y = 0}, 1),
         "a cell outside the map is rejected");
  map.SetTerrain(dnd::Cell{.x = 3, .y = 3}, dnd::Terrain::kWall);
  Expect(!dnd::CanPlaceToken(map, tokens, dnd::Cell{.x = 3, .y = 3}, 1),
         "a wall cell is rejected");

  Expect(dnd::MoveTokenToCell(tokens, map, 1, dnd::Cell{.x = 0, .y = 0}),
         "a free cell moves the token");
  Expect(tokens[0].cell == dnd::Cell{.x = 0, .y = 0}, "the token moved");
  Expect(!dnd::MoveTokenToCell(tokens, map, 1, dnd::Cell{.x = 2, .y = 1}),
         "a move onto another token is refused");
  Expect(tokens[0].cell == dnd::Cell{.x = 0, .y = 0},
         "a refused move keeps the position");
  Expect(!dnd::MoveTokenToCell(tokens, map, 1, dnd::Cell{.x = 0, .y = 0}),
         "a move to the same cell changes nothing");
  Expect(!dnd::MoveTokenToCell(tokens, map, 9, dnd::Cell{.x = 0, .y = 2}),
         "an unknown token does not move");

  Expect(dnd::TrimTokenName("  Гоблин \t") == "Гоблин",
         "a token name is trimmed");
  Expect(dnd::TrimTokenName("   ").empty(), "a blank token name is empty");
}

auto TestSceneDeleteRules() -> void {
  dnd::Campaign campaign = dnd::MakeCampaign(dnd::GameSnapshot{}, "Одна");
  Expect(!dnd::CanDeleteScene(campaign),
         "the last scene of a campaign cannot be deleted");
  campaign.scenes.push_back(dnd::Scene{.id = dnd::NextSceneId(campaign),
                                       .name = "Вторая",
                                       .note = "",
                                       .snapshot = {}});
  Expect(dnd::CanDeleteScene(campaign),
         "a campaign with two scenes allows deletion");
}

auto TestChatInput() -> void {
  dnd::ChatInput chat;
  Expect(!chat.Active() && chat.Text().empty(), "chat starts closed");
  chat.Open();
  Expect(chat.Active() && chat.Text().empty(), "Enter opens an empty line");
  chat.Append("привет");
  Expect(chat.Text() == "привет", "chat accepts Cyrillic text");
  chat.Backspace();
  Expect(chat.Text() == "приве", "backspace removes a whole code point");
  chat.Backspace();
  Expect(chat.Text() == "прив", "backspace keeps the line valid");

  chat.OpenWith("/aoe ");
  Expect(chat.Active() && chat.Text() == "/aoe ",
         "the area template primes the damage command");
  const std::string taken = chat.Take();
  Expect(taken == "/aoe " && chat.Text().empty() && chat.Active(),
         "taking the line clears it and keeps the input open");
  chat.Close();
  Expect(!chat.Active() && chat.Text().empty(), "Esc closes and clears");

  chat.Open();
  for (int i = 0; i < 100; ++i) {
    chat.Append("я");
  }
  Expect(chat.Text().size() == dnd::kMaxChatInputBytes,
         "chat stops at the byte limit");
  chat.Append("я");
  Expect(chat.Text().size() == dnd::kMaxChatInputBytes &&
             dnd::PreviousCodePointStart(chat.Text()) ==
                 dnd::kMaxChatInputBytes - 2,
         "the byte limit never splits a Cyrillic letter");
}

}  // namespace

auto main() -> int {
  try {
    TestGridMap();
    TestCellPropsMovement();
    TestCover();
    TestHazardsAndCurrent();
    TestCurrentShift();
    TestMoveRules();
    TestFogLineOfSight();
    TestFogDiagonalSeal();
    TestFogExploredMemory();
    TestLightField();
    TestDarkvisionAndSmoke();
    TestTokenLight();
    TestBlindedCharacterKeepsOwnFog();
    TestServerVisibility();
    TestPendingMove();
    TestOwnership();
    TestClaimAndTokenAdmin();
    TestDamageHealRest();
    TestShortRestHitDice();
    TestRestChoicesAndClock();
    TestConcentration();
    TestAttack();
    TestAoeDamage();
    TestDeathSaves();
    TestConditionDurationAndInitiative();
    TestConditionSaves();
    TestConditionPaletteAndExhaustion();
    TestConditionAttackModifiers();
    TestDamageTypesAndDefenses();
    TestRangedAttackAndSurprise();
    TestDiceExpression();
    TestBestiary();
    TestDynamicFogObservers();
    TestFogSurvivesReset();
    TestSeparateVisibility();
    TestStaleFogRecord();
    TestIncrementalView();
    TestRevealedAreas();
    TestFogMovement();
    TestLevels();
    TestExperience();
    TestUtf8();
    TestPlayerNames();
    TestVersion();
    TestUpdateManifest();
    TestUpdateAssetPlan();
    TestUpdateAdvice();
    TestStartupRecovery();
    TestCaseInsensitiveOwnership();
    TestTokenManagement();
    TestEncounter();
    TestEncounterComposition();
    TestEncounterCommands();
    TestMovementBudget();
    TestTurnEconomy();
    TestMovementPathsAndDiagonals();
    TestCrawlJumpFall();
    TestAoe();
    TestAoeCommands();
    TestChatAndDice();
    TestChatCommandParsing();
    TestPing();
    TestMapBackground();
    TestTokenPortrait();
    TestCommandEventSerialization();
    TestSaveLoadRoundtrip();
    TestReadsPreviousVersions();
    TestCampaign();
    TestCampaignSaver();
    TestSceneIds();
    TestOverlayMerge();
    TestPartySceneDelete();
    TestOverlayRoundtrip();
    TestCampaignUntouched();
    TestOverlayDirty();
    TestOverlaySaver();
    TestDamagedPayloads();
    TestSessionLog();
    TestSessionLogHistory();
    TestSha256();
    TestX25519();
    TestChaCha20();
    TestHkdf();
    TestHandshakeProofs();
    TestSessionCipher();
    TestCamera();
    TestAutosave();
    TestLogScroll();
    TestDeleteConfirm();
    TestStatusMessage();
    TestMenuModel();
    TestBottomHints();
    TestNavigation();
    TestEditorPaint();
    TestEditorTokenMove();
    TestSceneDeleteRules();
    TestChatInput();
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
