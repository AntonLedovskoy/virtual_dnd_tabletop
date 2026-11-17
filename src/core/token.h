#pragma once

#include <cstdint>
#include <string>

#include "core/grid_map.h"

namespace dnd {

using TokenId = std::uint32_t;

// Player tokens provide party vision. NPCs and monsters are only visible where
// a player can already see. The kind also decides which tokens players may
// claim as their character.
enum class TokenKind : std::uint8_t {
  kPlayer = 0,
  kNpc = 1,
  kMonster = 2,
};

struct Token {
  TokenId id = 0;
  std::string name;
  // Empty owner means a free/party token any player may move; the DM can
  // always move every token.
  std::string owner;
  int hp = 10;
  int max_hp = 10;
  int initiative = 0;
  TokenKind kind = TokenKind::kMonster;
  // 0xRRGGBB, or 0 to derive the colour from the token id.
  std::uint32_t color = 0;
  Cell cell{};
};

}  // namespace dnd
