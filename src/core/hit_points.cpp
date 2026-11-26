#include "core/hit_points.h"

#include <algorithm>

#include "core/levels.h"

namespace dnd {

auto CharacterMaxHp(int level, int hit_die, int con_modifier) -> int {
  const int resolved_level = std::clamp(level, 1, kMaxLevel);
  const int first_level = hit_die + con_modifier;
  const int later_levels =
      (resolved_level - 1) * HitPointsPerLevel(hit_die, con_modifier);
  return std::max(1, first_level + later_levels);
}

auto MonsterMaxHp(int hit_dice_count, int hit_die, int con_modifier) -> int {
  const int dice = std::max(1, hit_dice_count);
  const int average = (dice * (hit_die + 1)) / 2;
  return std::max(1, average + (dice * con_modifier));
}

auto AutoMaxHp(const Token& token) -> int {
  if (token.kind == TokenKind::kPlayer) {
    return CharacterMaxHp(LevelForXp(token.xp), token.hit_die,
                          token.con_modifier);
  }
  return MonsterMaxHp(token.hit_dice_count, token.hit_die, token.con_modifier);
}

auto ApplyMaxHpChange(Token& token, int max_hp) -> void {
  const int resolved_max = std::max(1, max_hp);
  if (token.hp > 0) {
    token.hp =
        std::clamp(token.hp + (resolved_max - token.max_hp), 1, resolved_max);
  }
  token.max_hp = resolved_max;
}

auto RecomputeAutoHp(Token& token) -> void {
  if (!token.auto_hp) {
    return;
  }
  ApplyMaxHpChange(token, AutoMaxHp(token));
}

}  // namespace dnd
