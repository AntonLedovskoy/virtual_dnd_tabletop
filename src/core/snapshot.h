#pragma once

#include <vector>

#include "core/encounter.h"
#include "core/grid_map.h"
#include "core/token.h"

namespace dnd {

// Complete authoritative world state: enough to rebuild a GameState from
// scratch, both for joining clients and for save files.
struct GameSnapshot {
  GridMap map{1, 1};
  std::vector<Token> tokens;
  TokenId next_token_id = 1;
  Encounter encounter;
};

}  // namespace dnd
