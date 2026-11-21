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

// Бонус мастерства по уровню (правила 2024): +2 на 1–4, +3 на 5–8,
// +4 на 9–12, +5 на 13–16, +6 на 17–20.
[[nodiscard]] auto ProficiencyBonus(int level) -> int;

// Кость хитов, допустимая у персонажа: d6, d8, d10 или d12.
[[nodiscard]] auto IsValidHitDie(int hit_die) -> bool;

// Средний прирост максимума HP за уровень выше первого (2024, «среднее
// значение»): d6 — 4, d8 — 5, d10 — 6, d12 — 7.
[[nodiscard]] auto HitDieAverage(int hit_die) -> int;

// Прирост максимума HP за один новый уровень: среднее кости хитов плюс
// модификатор Телосложения, но не меньше 1 (правило 2024).
[[nodiscard]] auto HitPointsPerLevel(int hit_die, int con_modifier) -> int;

// Запас костей хитов персонажа равен его уровню (1..kMaxLevel).
[[nodiscard]] auto HitDiceMaximum(int level) -> int;

// Сколько костей хитов осталось: запас минус потраченные, не меньше нуля.
[[nodiscard]] auto HitDiceAvailable(int level, int spent) -> int;

// Лечение за одну потраченную кость хитов (2024): бросок кости плюс
// модификатор Телосложения, но не меньше нуля.
[[nodiscard]] auto HitDieHeal(int roll, int con_modifier) -> int;

// На этих уровнях персонаж получает повышение характеристик (2024).
[[nodiscard]] auto IsAbilityScoreLevel(int level) -> bool;
// Эпический дар выдаётся на 19 уровне (2024).
inline constexpr int kEpicBoonLevel = 19;

}  // namespace dnd
