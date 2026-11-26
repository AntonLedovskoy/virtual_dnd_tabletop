#include "core/ownership.h"

#include "core/player_name.h"

namespace dnd {

auto IsDmOwnedCharacter(const Token& token) -> bool {
  return token.kind == TokenKind::kPlayer &&
         SamePlayerName(token.owner, kDmOwner);
}

auto FollowsPlayerRules(const Token& token, bool can_control_all) -> bool {
  return !can_control_all || IsDmOwnedCharacter(token);
}

}  // namespace dnd
