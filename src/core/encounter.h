#pragma once

#include <cstddef>
#include <vector>

#include "core/token.h"

namespace dnd {

// Server-authoritative combat order. `order` holds token ids sorted by
// initiative; `turn_index` points at the token whose turn it is.
struct Encounter {
  bool active = false;
  int round = 0;
  std::vector<TokenId> order;
  std::size_t turn_index = 0;
};

}  // namespace dnd
