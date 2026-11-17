#include "core/dice.h"

#include <algorithm>
#include <cctype>
#include <charconv>

namespace dnd {
namespace {

constexpr int kMaxDiceCount = 20;
constexpr int kMinDiceSides = 2;
constexpr int kMaxDiceSides = 100;
constexpr int kMaxModifier = 100;

auto Trim(std::string_view text) -> std::string_view {
  while (!text.empty() &&
         std::isspace(static_cast<unsigned char>(text.front()))) {
    text.remove_prefix(1);
  }
  while (!text.empty() &&
         std::isspace(static_cast<unsigned char>(text.back()))) {
    text.remove_suffix(1);
  }
  return text;
}

auto StartsWithIgnoreCase(std::string_view text, std::string_view prefix)
    -> bool {
  if (text.size() < prefix.size()) {
    return false;
  }
  for (std::size_t i = 0; i < prefix.size(); ++i) {
    const auto left = static_cast<unsigned char>(text[i]);
    const auto right = static_cast<unsigned char>(prefix[i]);
    if (std::tolower(left) != std::tolower(right)) {
      return false;
    }
  }
  return true;
}

auto ParseInt(std::string_view text, int& value) -> bool {
  if (text.empty()) {
    return false;
  }
  int parsed = 0;
  const auto result =
      std::from_chars(text.data(), text.data() + text.size(), parsed);
  if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) {
    return false;
  }
  value = parsed;
  return true;
}

auto ParseSignedInt(std::string_view text, int& value) -> bool {
  if (text.empty()) {
    return false;
  }
  if (text.front() == '+') {
    text.remove_prefix(1);
  }
  return ParseInt(text, value);
}

auto ModifierText(int modifier) -> std::string {
  if (modifier > 0) {
    return " + " + std::to_string(modifier);
  }
  if (modifier < 0) {
    return " - " + std::to_string(-modifier);
  }
  return {};
}

}  // namespace

auto ParseDiceExpression(std::string_view text, DiceExpression& expression)
    -> bool {
  text = Trim(text);
  if (text.empty()) {
    return false;
  }

  bool advantage = false;
  bool disadvantage = false;
  if (StartsWithIgnoreCase(text, "adv")) {
    advantage = true;
    text = Trim(text.substr(3));
  } else if (StartsWithIgnoreCase(text, "dis")) {
    disadvantage = true;
    text = Trim(text.substr(3));
  }
  if (text.empty()) {
    return false;
  }

  const auto separator = text.find_first_of("dD");
  if (separator == std::string_view::npos) {
    return false;
  }

  int count = 1;
  if (separator > 0 && !ParseInt(text.substr(0, separator), count)) {
    return false;
  }
  if (count < 1 || count > kMaxDiceCount) {
    return false;
  }

  std::string_view sides_text = text.substr(separator + 1);
  std::size_t modifier_pos = sides_text.find_first_of("+-");
  std::string_view sides_part = sides_text.substr(0, modifier_pos);
  int sides = 0;
  if (!ParseInt(sides_part, sides) || sides < kMinDiceSides ||
      sides > kMaxDiceSides) {
    return false;
  }

  int modifier = 0;
  if (modifier_pos != std::string_view::npos &&
      !ParseSignedInt(sides_text.substr(modifier_pos), modifier)) {
    return false;
  }
  if (modifier < -kMaxModifier || modifier > kMaxModifier) {
    return false;
  }
  if (advantage && disadvantage) {
    return false;
  }
  if ((advantage || disadvantage) && (count != 1 || sides != 20)) {
    return false;
  }

  expression = DiceExpression{
      .count = count,
      .sides = sides,
      .modifier = modifier,
      .advantage = advantage,
      .disadvantage = disadvantage,
  };
  return true;
}

auto RollDiceExpression(const DiceExpression& expression, std::mt19937& rng)
    -> DiceRollResult {
  std::uniform_int_distribution<int> distribution(1, expression.sides);
  DiceRollResult result;
  result.rolls.reserve(static_cast<std::size_t>(expression.count));

  if (expression.advantage || expression.disadvantage) {
    const int first = distribution(rng);
    const int second = distribution(rng);
    result.rolls = {first, second};
    result.chosen = expression.advantage ? std::max(first, second)
                                         : std::min(first, second);
    result.total = result.chosen + expression.modifier;
    result.breakdown = std::to_string(first) + ", " + std::to_string(second) +
                       " → " + std::to_string(result.chosen) +
                       ModifierText(expression.modifier);
    return result;
  }

  int sum = 0;
  std::string breakdown;
  for (int i = 0; i < expression.count; ++i) {
    const int roll = distribution(rng);
    result.rolls.push_back(roll);
    sum += roll;
    if (i > 0) {
      breakdown += " + ";
    }
    breakdown += std::to_string(roll);
  }
  breakdown += ModifierText(expression.modifier);
  result.chosen = sum;
  result.total = sum + expression.modifier;
  result.breakdown = std::move(breakdown);
  return result;
}

}  // namespace dnd
