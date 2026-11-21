#pragma once

namespace dnd {

// Уровни персонажа. Опыт хранится в токене, уровень всегда вычисляется по
// порогам, чтобы два поля не могли разойтись.
inline constexpr int kMaxLevel = 20;
// Верхняя граница хранимого опыта: дальше уровень всё равно не растёт.
inline constexpr int kMaxXp = 1000000;

// Порог опыта, с которого начинается уровень (1..kMaxLevel).
[[nodiscard]] auto XpForLevel(int level) -> int;
// Уровень для накопленного опыта: 1..kMaxLevel, отрицательный опыт — 1.
[[nodiscard]] auto LevelForXp(int xp) -> int;

}  // namespace dnd
