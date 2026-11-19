#pragma once

#include <cstdint>
#include <string>
#include <utility>
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

// Состояние токена. rounds > 0 — сколько ходов токена ещё действует,
// 0 — до ручного снятия (например, «без сознания»).
struct Condition {
  std::string name;
  int rounds = 0;

  Condition() = default;
  explicit Condition(std::string condition_name, int remaining = 0)
      : name(std::move(condition_name)), rounds(remaining) {}
  explicit Condition(const char* condition_name, int remaining = 0)
      : name(condition_name), rounds(remaining) {}
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
  int initiative_bonus = 0;
  std::string damage;
  std::vector<Condition> conditions;
  // Израсходованный за ход запас движения в клетках. Сбрасывается в начале
  // хода токена и вне боя не накапливается.
  int movement_used = 0;
  // Спасброски от смерти (0..3); имеют смысл только при hp == 0.
  int death_save_successes = 0;
  int death_save_failures = 0;
  TokenKind kind = TokenKind::kMonster;
  // 0xRRGGBB или 0, чтобы брать цвет из id токена.
  std::uint32_t color = 0;
  Cell cell{};
};

}  // namespace dnd
