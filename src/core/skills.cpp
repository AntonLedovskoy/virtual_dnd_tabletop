#include "core/skills.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <string>
#include <vector>

#include "core/conditions.h"
#include "core/levels.h"
#include "core/player_name.h"

namespace dnd {
namespace {

struct SkillInfo {
  Skill skill;
  Ability ability;
  const char* name;
  const char* english;
};

constexpr std::array<SkillInfo, kSkillCount> kSkills{{
    {Skill::kAcrobatics, Ability::kDexterity, "Акробатика", "acrobatics"},
    {Skill::kAnimalHandling, Ability::kWisdom, "Уход за животными",
     "animal handling"},
    {Skill::kArcana, Ability::kIntelligence, "Магия", "arcana"},
    {Skill::kAthletics, Ability::kStrength, "Атлетика", "athletics"},
    {Skill::kDeception, Ability::kCharisma, "Обман", "deception"},
    {Skill::kHistory, Ability::kIntelligence, "История", "history"},
    {Skill::kInsight, Ability::kWisdom, "Проницательность", "insight"},
    {Skill::kIntimidation, Ability::kCharisma, "Запугивание", "intimidation"},
    {Skill::kInvestigation, Ability::kIntelligence, "Расследование",
     "investigation"},
    {Skill::kMedicine, Ability::kWisdom, "Медицина", "medicine"},
    {Skill::kNature, Ability::kIntelligence, "Природа", "nature"},
    {Skill::kPerception, Ability::kWisdom, "Восприятие", "perception"},
    {Skill::kPerformance, Ability::kCharisma, "Выступление", "performance"},
    {Skill::kPersuasion, Ability::kCharisma, "Убеждение", "persuasion"},
    {Skill::kReligion, Ability::kIntelligence, "Религия", "religion"},
    {Skill::kSleightOfHand, Ability::kDexterity, "Ловкость рук",
     "sleight of hand"},
    {Skill::kStealth, Ability::kDexterity, "Скрытность", "stealth"},
    {Skill::kSurvival, Ability::kWisdom, "Выживание", "survival"},
}};

constexpr std::array<const char*, kAbilityCount> kAbilityNames{
    "Сила", "Ловкость", "Телосложение", "Интеллект", "Мудрость", "Харизма",
};

auto Trim(std::string_view text) -> std::string_view {
  while (!text.empty() &&
         std::isspace(static_cast<unsigned char>(text.front())) != 0) {
    text.remove_prefix(1);
  }
  while (!text.empty() &&
         std::isspace(static_cast<unsigned char>(text.back())) != 0) {
    text.remove_suffix(1);
  }
  return text;
}

}  // namespace

auto SkillName(Skill skill) -> std::string_view {
  if (!IsValidSkill(skill)) {
    return {};
  }
  return kSkills[static_cast<std::size_t>(skill)].name;
}

auto SkillAbility(Skill skill) -> Ability {
  if (!IsValidSkill(skill)) {
    return Ability::kWisdom;
  }
  return kSkills[static_cast<std::size_t>(skill)].ability;
}

auto AbilityName(Ability ability) -> std::string_view {
  const auto index = static_cast<std::size_t>(ability);
  if (index >= kAbilityNames.size()) {
    return {};
  }
  return kAbilityNames[index];
}

auto ParseSkill(std::string_view text) -> std::optional<Skill> {
  const std::string_view trimmed = Trim(text);
  if (trimmed.empty()) {
    return std::nullopt;
  }
  // Имена сравниваются без учёта регистра, в том числе для кириллицы.
  const std::string key = PlayerNameKey(trimmed);
  for (const SkillInfo& info : kSkills) {
    if (PlayerNameKey(info.name) == key || PlayerNameKey(info.english) == key) {
      return info.skill;
    }
  }
  return std::nullopt;
}

auto IsSkillProficient(const Token& token, Skill skill) -> bool {
  return IsValidSkill(skill) &&
         (token.skill_proficiency & SkillBit(skill)) != 0;
}

auto SkillCheckBonus(const Token& token, Skill skill) -> int {
  int bonus = token.skill_bonus;
  if (IsSkillProficient(token, skill)) {
    bonus += ProficiencyBonus(LevelForXp(token.xp));
  }
  return bonus - ExhaustionPenalty(token);
}

auto PassiveSkillScore(const Token& token, Skill skill, bool disadvantage)
    -> int {
  const int bonus = IsValidSkill(skill) ? SkillCheckBonus(token, skill) : 0;
  return 10 + bonus + (disadvantage ? -5 : 0);
}

auto FormatSkillMask(std::uint32_t mask) -> std::string {
  std::string text;
  for (const SkillInfo& info : kSkills) {
    if ((mask & SkillBit(info.skill)) == 0) {
      continue;
    }
    if (!text.empty()) {
      text += ", ";
    }
    text += info.name;
  }
  return text;
}

auto ParseSkillMask(std::string_view text) -> std::optional<std::uint32_t> {
  std::string_view rest = Trim(text);
  const std::string key = PlayerNameKey(rest);
  if (key.empty() || key == "—" || key == "-" || key == "нет" || key == "off" ||
      key == "none") {
    return 0;
  }
  std::uint32_t mask = 0;
  while (!rest.empty()) {
    const std::size_t comma = rest.find(',');
    const std::string_view item =
        comma == std::string_view::npos ? rest : rest.substr(0, comma);
    const std::optional<Skill> skill = ParseSkill(item);
    if (!skill.has_value()) {
      return std::nullopt;
    }
    mask |= SkillBit(*skill);
    if (comma == std::string_view::npos) {
      break;
    }
    rest = Trim(rest.substr(comma + 1));
  }
  return mask;
}

}  // namespace dnd
