#include "core/editor_token.h"

#include <algorithm>

namespace dnd {

auto CanPlaceToken(const GridMap& map, const std::vector<Token>& tokens,
                   Cell cell, TokenId moving_id) -> bool {
  if (!map.Contains(cell) || map.IsBlocked(cell)) {
    return false;
  }
  return std::ranges::all_of(
      tokens, [cell, moving_id](const Token& token) -> bool {
        return token.id == moving_id || token.cell != cell;
      });
}

auto MoveTokenToCell(std::vector<Token>& tokens, const GridMap& map, TokenId id,
                     Cell cell) -> bool {
  if (!CanPlaceToken(map, tokens, cell, id)) {
    return false;
  }
  for (Token& token : tokens) {
    if (token.id != id) {
      continue;
    }
    if (token.cell == cell) {
      return false;
    }
    token.cell = cell;
    return true;
  }
  return false;
}

auto TrimTokenName(std::string_view name) -> std::string {
  while (!name.empty() && (name.front() == ' ' || name.front() == '\t')) {
    name.remove_prefix(1);
  }
  while (!name.empty() && (name.back() == ' ' || name.back() == '\t')) {
    name.remove_suffix(1);
  }
  return std::string(name);
}

}  // namespace dnd
