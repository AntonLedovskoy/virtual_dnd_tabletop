#pragma once

#include <cstdint>
#include <string>

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
  int initiative = 0;
  int ac = 10;
  int speed = 30;
  std::string notes;
  TokenKind kind = TokenKind::kMonster;
  // 0xRRGGBB или 0, чтобы брать цвет из id токена.
  std::uint32_t color = 0;
  Cell cell{};
};

}  // namespace dnd
