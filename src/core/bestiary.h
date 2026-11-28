#pragma once

#include <random>
#include <string>
#include <vector>

#include "core/damage.h"
#include "core/grid_map.h"
#include "core/hit_points.h"
#include "core/player_name.h"
#include "core/token.h"

namespace dnd {

// Размер группы монстров при спавне: от 3 до 5 одинаковых.
inline constexpr int kMinMonsterGroup = 3;
inline constexpr int kMaxMonsterGroup = 5;

// Небольшая встроенная библиотека монстров. При спавне шаблон копируется в
// токен, поэтому редактирование монстра не меняет шаблон.
struct MonsterTemplate {
  // Стабильный ключ записи контента: по нему файл перекрывает встроенный
  // шаблон, в снимок и протокол он не попадает.
  std::string id;
  std::string name;
  int ac = 10;
  // HP выводятся из костей, как в блоке статов SRD (core/hit_points).
  int hit_die = 8;
  int con_modifier = 0;
  int hit_dice_count = 1;
  int speed = 9;
  int attack_bonus = 0;
  int save_bonus = 0;
  std::string damage;
  std::string notes;
  // Дальнобойная атака в клетках (0 — без ограничения) и защита от урона.
  int attack_range = 0;
  std::uint32_t resist = 0;
  std::uint32_t vulnerable = 0;
  std::uint32_t immune = 0;
};

[[nodiscard]] auto DefaultBestiary() -> const std::vector<MonsterTemplate>&;

// Один путь спавна для игры и редактора: токен-монстр из шаблона. Копирует
// все статы, включая дальность и защиты, и считает HP по костям.
[[nodiscard]] auto TokenFromTemplate(const MonsterTemplate& monster, TokenId id,
                                     Cell cell) -> Token;

// Сколько монстров в группе: kMinMonsterGroup..kMaxMonsterGroup.
[[nodiscard]] auto MonsterGroupSize(std::mt19937& rng) -> int;

// Клетки под группу: origin и свободные клетки вокруг по возрастанию
// расстояния. Стены и занятые токенами клетки пропускаются; если места
// меньше count, возвращается сколько есть.
[[nodiscard]] auto MonsterGroupCells(const GridMap& map, Cell origin, int count,
                                     const std::vector<Token>& tokens)
    -> std::vector<Cell>;

}  // namespace dnd
