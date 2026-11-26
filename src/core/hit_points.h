#pragma once

#include "core/token.h"

namespace dnd {

// Максимум HP персонажа по правилам 2024: на первом уровне — максимум кости
// хитов плюс модификатор Телосложения, за каждый следующий уровень — среднее
// кости плюс Телосложение (не меньше единицы, core/levels.h).
[[nodiscard]] auto CharacterMaxHp(int level, int hit_die, int con_modifier)
    -> int;

// Максимум HP монстра из блока статов: кости хитов, среднее вниз
// (floor(костей × (кость + 1) / 2)) плюс костей × модификатор Телосложения.
// Число костей меньше единицы считается единицей.
[[nodiscard]] auto MonsterMaxHp(int hit_dice_count, int hit_die,
                                int con_modifier) -> int;

// Максимум HP, который следует из костей, Телосложения и уровня токена
// (у персонажей уровень выводится из опыта, у монстров — из числа костей).
[[nodiscard]] auto AutoMaxHp(const Token& token) -> int;

// Меняет максимум HP и сдвигает текущие HP на ту же дельту: раненый персонаж
// остаётся в сознании хотя бы с 1 HP, лежачий на нуле не поднимается.
auto ApplyMaxHpChange(Token& token, int max_hp) -> void;

// Пересчитывает максимум HP авто-токена по костям (инвариант «авто = по
// кости»). Ручной максимум не трогает.
auto RecomputeAutoHp(Token& token) -> void;

}  // namespace dnd
