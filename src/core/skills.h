#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "core/token.h"

namespace dnd {

// Характеристики и навыки 2024 (SRD 5.2.1, «Использование способностей»).
// Модификаторы характеристик в токене не хранятся: лист держит один общий
// модификатор проверок (`Token::skill_bonus`), а владение навыком добавляет
// бонус мастерства. Это то же упрощение, что один спасбросок на все
// способности, и записано в AGENTS.md.
enum class Ability : std::uint8_t {
  kStrength = 0,
  kDexterity = 1,
  kConstitution = 2,
  kIntelligence = 3,
  kWisdom = 4,
  kCharisma = 5,
};

inline constexpr int kAbilityCount = 6;

enum class Skill : std::uint8_t {
  kAcrobatics = 0,
  kAnimalHandling = 1,
  kArcana = 2,
  kAthletics = 3,
  kDeception = 4,
  kHistory = 5,
  kInsight = 6,
  kIntimidation = 7,
  kInvestigation = 8,
  kMedicine = 9,
  kNature = 10,
  kPerception = 11,
  kPerformance = 12,
  kPersuasion = 13,
  kReligion = 14,
  kSleightOfHand = 15,
  kStealth = 16,
  kSurvival = 17,
};

inline constexpr int kSkillCount = 18;
// Маска владения навыками: бит на каждый навык.
inline constexpr std::uint32_t kAllSkills = (1U << kSkillCount) - 1U;
// Владения в листе не длиннее заметок: список имён.
inline constexpr std::size_t kMaxSkillListBytes = 256;
// Радиус поиска: 30 футов, то есть 6 клеток.
inline constexpr int kSearchRadiusCells = 6;
// КС скрытности и проверки «Поиск»/«Изучение» по умолчанию (2024, действие
// Hide: проверка Ловкости (Скрытность) против КС 15).
inline constexpr int kDefaultSkillDc = 15;

[[nodiscard]] constexpr auto IsValidSkill(Skill skill) -> bool {
  return static_cast<std::uint8_t>(skill) <
         static_cast<std::uint8_t>(kSkillCount);
}

[[nodiscard]] constexpr auto IsValidSkillMask(std::uint32_t mask) -> bool {
  return (mask & ~kAllSkills) == 0;
}

[[nodiscard]] constexpr auto SkillBit(Skill skill) -> std::uint32_t {
  return 1U << static_cast<std::uint8_t>(skill);
}

// Русское имя навыка и его характеристика.
[[nodiscard]] auto SkillName(Skill skill) -> std::string_view;
[[nodiscard]] auto SkillAbility(Skill skill) -> Ability;
[[nodiscard]] auto AbilityName(Ability ability) -> std::string_view;
// Разбор имени навыка по-русски или по-английски, без учёта регистра.
[[nodiscard]] auto ParseSkill(std::string_view text) -> std::optional<Skill>;

[[nodiscard]] auto IsSkillProficient(const Token& token, Skill skill) -> bool;

// Модификатор проверки: общий бонус листа плюс бонус мастерства за владение
// минус истощение. Так же, как attack_bonus/save_bonus, лист хранит основу,
// а бонус мастерства добавляется по уровню.
[[nodiscard]] auto SkillCheckBonus(const Token& token, Skill skill) -> int;

// Пассивное значение навыка (пассивное Восприятие): 10 + модификатор.
// disadvantage — помеха на проверку (тусклый свет для зрения), тогда −5.
[[nodiscard]] auto PassiveSkillScore(const Token& token, Skill skill,
                                     bool disadvantage = false) -> int;

// Список владений в листе: «Восприятие, Скрытность». Пустая строка — ничего.
[[nodiscard]] auto FormatSkillMask(std::uint32_t mask) -> std::string;
// Разбор того же списка; «—», «нет», «off» и пустая строка дают 0.
[[nodiscard]] auto ParseSkillMask(std::string_view text)
    -> std::optional<std::uint32_t>;

}  // namespace dnd
