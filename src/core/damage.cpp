#include "core/damage.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <iterator>
#include <ranges>

namespace dnd {
namespace {

struct DamageTypeNameEntry {
  DamageType type;
  std::string_view name;
  std::string_view english;
};

// Порядок совпадает со значениями enum: индекс — static_cast типа.
constexpr std::array<DamageTypeNameEntry, kDamageTypeCount> kDamageTypeNames{{
    {DamageType::kNone, "", ""},
    {DamageType::kBludgeoning, "дробящий", "bludgeoning"},
    {DamageType::kPiercing, "колющий", "piercing"},
    {DamageType::kSlashing, "рубящий", "slashing"},
    {DamageType::kAcid, "кислота", "acid"},
    {DamageType::kCold, "холод", "cold"},
    {DamageType::kFire, "огонь", "fire"},
    {DamageType::kForce, "силовой", "force"},
    {DamageType::kLightning, "электричество", "lightning"},
    {DamageType::kNecrotic, "некротический", "necrotic"},
    {DamageType::kPoison, "яд", "poison"},
    {DamageType::kPsychic, "психический", "psychic"},
    {DamageType::kRadiant, "излучение", "radiant"},
    {DamageType::kThunder, "звук", "thunder"},
}};

constexpr auto IsSpace(char symbol) -> bool {
  return symbol == ' ' || symbol == '\t' || symbol == '\n' || symbol == '\r';
}

auto Trim(std::string_view text) -> std::string_view {
  while (!text.empty() && IsSpace(text.front())) {
    text.remove_prefix(1);
  }
  while (!text.empty() && IsSpace(text.back())) {
    text.remove_suffix(1);
  }
  return text;
}

}  // namespace

auto DamageTypeName(DamageType type) -> std::string_view {
  const auto index = static_cast<std::size_t>(type);
  if (index >= kDamageTypeNames.size()) {
    return {};
  }
  return kDamageTypeNames[index].name;
}

auto ParseDamageType(std::string_view text) -> std::optional<DamageType> {
  const std::string_view trimmed = Trim(text);
  for (const DamageTypeNameEntry& entry : kDamageTypeNames) {
    if (entry.type == DamageType::kNone) {
      continue;
    }
    if (trimmed == entry.name || trimmed == entry.english) {
      return entry.type;
    }
  }
  return std::nullopt;
}

auto ParseDamageTypeList(std::string_view text, std::uint32_t& mask) -> bool {
  std::uint32_t parsed = 0;
  std::size_t start = 0;
  while (start < text.size()) {
    while (start < text.size() &&
           (IsSpace(text[start]) || text[start] == ',' || text[start] == ';')) {
      ++start;
    }
    std::size_t end = start;
    while (end < text.size() && !IsSpace(text[end]) && text[end] != ',' &&
           text[end] != ';') {
      ++end;
    }
    if (end > start) {
      const std::optional<DamageType> type =
          ParseDamageType(text.substr(start, end - start));
      if (!type.has_value()) {
        return false;
      }
      parsed |= DamageTypeBit(*type);
    }
    start = end;
  }
  mask = parsed;
  return true;
}

auto FormatDamageTypeList(std::uint32_t mask) -> std::string {
  std::string text;
  for (int index = 1; index < kDamageTypeCount; ++index) {
    const auto type = static_cast<DamageType>(index);
    if ((mask & DamageTypeBit(type)) == 0) {
      continue;
    }
    if (!text.empty()) {
      text += ", ";
    }
    text += DamageTypeName(type);
  }
  return text;
}

auto AdjustedDamage(int amount, DamageType type, const DamageProfile& profile)
    -> int {
  if (amount <= 0 || type == DamageType::kNone) {
    return amount;
  }
  const std::uint32_t bit = DamageTypeBit(type);
  if ((profile.immune & bit) != 0) {
    return 0;
  }
  int result = amount;
  if ((profile.resist & bit) != 0) {
    result /= 2;
  }
  if ((profile.vulnerable & bit) != 0) {
    result *= 2;
  }
  return result;
}

auto ParseDamageProfile(std::string_view text, DiceExpression& expression,
                        DamageType& type) -> bool {
  const std::string_view trimmed = Trim(text);
  if (trimmed.empty()) {
    return false;
  }
  if (ParseDiceExpression(trimmed, expression)) {
    type = DamageType::kNone;
    return true;
  }
  // Тип урона идёт последним словом: «1d8+2 огонь», «adv d20+5 яд».
  const std::size_t space = trimmed.find_last_of(" \t");
  if (space == std::string_view::npos || space + 1 >= trimmed.size()) {
    return false;
  }
  const std::optional<DamageType> parsed_type =
      ParseDamageType(trimmed.substr(space + 1));
  if (!parsed_type.has_value()) {
    return false;
  }
  const std::string_view dice = Trim(trimmed.substr(0, space));
  if (!ParseDiceExpression(dice, expression)) {
    return false;
  }
  type = *parsed_type;
  return true;
}

auto IsDamageProfilePrefix(std::string_view text) -> bool {
  if (text.size() > 96) {
    return false;
  }
  if (IsDiceExpressionPrefix(text)) {
    return true;
  }
  const std::size_t space = text.find_last_of(" \t");
  if (space == std::string_view::npos) {
    return false;
  }
  DiceExpression expression;
  if (!ParseDiceExpression(Trim(text.substr(0, space)), expression)) {
    return false;
  }
  const std::string_view tail = Trim(text.substr(space + 1));
  if (tail.empty()) {
    return true;
  }
  return std::ranges::any_of(kDamageTypeNames,
                             [tail](const DamageTypeNameEntry& entry) -> bool {
                               return entry.type != DamageType::kNone &&
                                      (entry.name.starts_with(tail) ||
                                       entry.english.starts_with(tail));
                             });
}

}  // namespace dnd
