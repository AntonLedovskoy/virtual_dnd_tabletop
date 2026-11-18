#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/grid_map.h"

namespace dnd {

using TokenId = std::uint32_t;

// Токены персонажей дают обзор партии. NPC и монстры видны только там,
// где игрок уже может видеть. Тип также определяет, каких токенов игроки
// могут забирать как своих персонажей.
enum class TokenKind : std::uint8_t {
  kPlayer = 0,
  kNpc = 1,
  kMonster = 2,
};

struct Token {
  TokenId id = 0;
  std::string name;
  // Пустой owner означает свободный/общий токен, который может двигать любой
  // игрок; DM всегда может двигать любой токен.
  std::string owner;
  int hp = 10;
  int max_hp = 10;
  int temp_hp = 0;
  int initiative = 0;
  int ac = 10;
  int speed = 9;
  std::string notes;
  int attack_bonus = 0;
  int save_bonus = 0;
  std::string damage;
  std::vector<std::string> conditions;
  TokenKind kind = TokenKind::kMonster;
  // 0xRRGGBB или 0, чтобы брать цвет из id токена.
  std::uint32_t color = 0;
  Cell cell{};
};

}  // namespace dnd
