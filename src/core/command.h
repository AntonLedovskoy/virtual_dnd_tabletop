#pragma once

#include <string>
#include <variant>

#include "core/encounter.h"
#include "core/grid_map.h"
#include "core/token.h"

namespace dnd {

// Client-to-server intents. The server validates and translates them into
// events; commands never mutate the client directly.
struct MoveToken {
  TokenId id = 0;
  Cell target{};
};

// Plain chat line, shown in the shared log.
struct Say {
  std::string text;
};

// Dice request, e.g. {count = 2, sides = 6} for 2d6. Rolled on the server so
// every client sees the same result.
struct RollDice {
  int count = 1;
  int sides = 20;
};

// DM-only: spawn a token (empty name gets a generated one). Player tokens are
// free by default and can then be claimed by a player.
struct AddToken {
  std::string name;
  Cell cell{};
  std::string owner;
  TokenKind kind = TokenKind::kMonster;
  int max_hp = 10;
};

// DM-only: delete a token.
struct RemoveToken {
  TokenId id = 0;
};

// Owner or DM: adjust a token.
struct SetTokenHp {
  TokenId id = 0;
  int hp = 0;
};

struct SetInitiative {
  TokenId id = 0;
  int value = 0;
};

// Player: take a free player token. The server fills in the actor name.
struct ClaimToken {
  TokenId id = 0;
};

// DM-only: assign or clear (empty owner) a token's owner.
struct SetTokenOwner {
  TokenId id = 0;
  std::string owner;
};

// Owner or DM: rename a token.
struct RenameToken {
  TokenId id = 0;
  std::string name;
};

// Owner or DM: change the HP maximum; current HP is clamped to it.
struct SetTokenMaxHp {
  TokenId id = 0;
  int max_hp = 10;
};

// Owner or DM: set a 0xRRGGBB token colour (0 restores the automatic colour).
struct SetTokenColor {
  TokenId id = 0;
  std::uint32_t color = 0;
};

// DM-only encounter control.
struct StartEncounter {};
struct EndEncounter {};
struct NextTurn {};

using Command =
    std::variant<MoveToken, Say, RollDice, AddToken, RemoveToken, SetTokenHp,
                 SetInitiative, ClaimToken, SetTokenOwner, RenameToken,
                 SetTokenMaxHp, SetTokenColor, StartEncounter, EndEncounter,
                 NextTurn>;

}  // namespace dnd
