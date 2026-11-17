#pragma once

#include <vector>

#include "core/encounter.h"
#include "core/grid_map.h"
#include "core/token.h"

namespace dnd {

// Полное авторитетное состояние мира: достаточно, чтобы собрать GameState
// заново — и для подключающихся клиентов, и для файлов сохранения.
struct GameSnapshot {
  GridMap map{1, 1};
  std::vector<Token> tokens;
  TokenId next_token_id = 1;
  Encounter encounter;
};

}  // namespace dnd
