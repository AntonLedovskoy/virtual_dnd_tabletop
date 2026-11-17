#pragma once

#include <variant>

#include "core/encounter.h"
#include "core/grid_map.h"
#include "core/log_line.h"
#include "core/snapshot.h"
#include "core/token.h"
#include "core/visibility.h"

namespace dnd {

// Server-to-client facts. Only events may change what a client displays, which
// keeps the local and the networked paths identical.
struct TokenMoved {
  TokenId id = 0;
  Cell from{};
  Cell to{};
};

// Refreshes the whole player view: fog plus exactly the tokens the player may
// see. Tokens can appear here for the first time (a monster stepping into the
// light) or disappear again, so a fog-only update is not enough.
struct ViewUpdated {
  GameSnapshot snapshot;
  FogSnapshot fog;
};

struct LogLineAdded {
  LogLine line;
};

// Incremental token updates. Players ignore the ones for tokens they cannot
// display, so these never leak hidden creatures.
struct TokenAdded {
  Token token;
};

struct TokenRemoved {
  TokenId id = 0;
};

struct TokenUpdated {
  Token token;
};

// Combat order changed (start, end, next turn).
struct EncounterUpdated {
  Encounter encounter;
};

using Event = std::variant<TokenMoved, ViewUpdated, LogLineAdded, TokenAdded,
                           TokenRemoved, TokenUpdated, EncounterUpdated>;

}  // namespace dnd
