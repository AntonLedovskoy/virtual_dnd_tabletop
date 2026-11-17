#pragma once

#include <vector>

#include "core/encounter.h"
#include "core/event.h"
#include "core/grid_map.h"
#include "core/log_line.h"
#include "core/snapshot.h"
#include "core/token.h"
#include "core/visibility.h"

namespace dnd {

// Everything the client is allowed to know: its own copy of the world plus the
// visibility slice received from the server. Rendering reads only from here.
class ClientView {
 public:
  ClientView();

  // Full (re)sync of world tokens and fog, e.g. on join or after loading a
  // save. The log history is kept.
  auto Reset(GameSnapshot snapshot, const FogSnapshot& fog) -> void;
  auto Apply(const Event& event) -> void;
  // Client-only feedback, never sent to other players.
  auto AddLocalLine(std::string text) -> void;

  [[nodiscard]] auto Map() const -> const GridMap& { return map_; }
  [[nodiscard]] auto Tokens() const -> const std::vector<Token>& {
    return tokens_;
  }
  [[nodiscard]] auto Fog() const -> const FogOfWar& { return fog_; }
  [[nodiscard]] auto Log() const -> const std::vector<LogLine>& { return log_; }
  [[nodiscard]] auto EncounterState() const -> const Encounter& {
    return encounter_;
  }

 private:
  static constexpr std::size_t kMaxLogLines = 200;

  auto FindToken(TokenId id) -> Token*;

  GridMap map_;
  std::vector<Token> tokens_;
  FogOfWar fog_;
  std::vector<LogLine> log_;
  Encounter encounter_;
};

}  // namespace dnd
