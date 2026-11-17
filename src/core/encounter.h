#pragma once

#include <cstddef>
#include <vector>

#include "core/token.h"

namespace dnd {

// Боевой порядок, которым управляет сервер. `order` хранит id токенов,
// отсортированные по инициативе; `turn_index` указывает на текущий токен.
struct Encounter {
  bool active = false;
  int round = 0;
  std::vector<TokenId> order;
  std::size_t turn_index = 0;
};

}  // namespace dnd
