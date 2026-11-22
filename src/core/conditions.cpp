#include "core/conditions.h"

#include <algorithm>

#include "core/token.h"

namespace dnd {
namespace {

auto Plural(int count, std::string_view one, std::string_view few,
            std::string_view many) -> std::string_view {
  const int last_two = count % 100;
  const int last = count % 10;
  if (last_two >= 11 && last_two <= 14) {
    return many;
  }
  if (last == 1) {
    return one;
  }
  if (last >= 2 && last <= 4) {
    return few;
  }
  return many;
}

auto FormatUnit(int count, std::string_view one, std::string_view few,
                std::string_view many) -> std::string {
  return std::to_string(count) + " " +
         std::string(Plural(count, one, few, many));
}

}  // namespace

auto FormatConditionDuration(int seconds) -> std::string {
  if (seconds <= 0) {
    return "до снятия";
  }
  if (seconds % kSecondsPerHour == 0) {
    return FormatUnit(seconds / kSecondsPerHour, "час", "часа", "часов");
  }
  if (seconds % kSecondsPerMinute == 0) {
    return FormatUnit(seconds / kSecondsPerMinute, "минута", "минуты", "минут");
  }
  if (seconds % kSecondsPerRound == 0) {
    return FormatUnit(seconds / kSecondsPerRound, "ход", "хода", "ходов");
  }
  return FormatUnit(seconds, "секунда", "секунды", "секунд");
}

auto ConcentrationDc(int damage) -> int {
  return std::min(30, std::max(10, damage / 2));
}

auto IsIncapacitatingCondition(std::string_view name) -> bool {
  return name == kUnconscious || name == kIncapacitated || name == kDead ||
         name == "парализован" || name == "оглушён" || name == "окаменел";
}

auto ConvertLegacyConditionRounds(std::vector<Condition>& conditions) -> void {
  for (Condition& condition : conditions) {
    if (condition.seconds > 0) {
      condition.seconds *= kSecondsPerRound;
    }
  }
}

auto DefaultConditions() -> const std::vector<std::string>& {
  // Константа названа по стилю проекта (kCamelCase), хотя clang-tidy для
  // локальных констант ждёт lower_case.
  // NOLINTNEXTLINE(readability-identifier-naming)
  static const std::vector<std::string> kConditions{
      "сбит с ног",
      "отравлен",
      "испуган",
      "захвачен",
      "опутан",
      "оглушён",
      std::string(kUnconscious),
      std::string(kConcentration),
  };
  return kConditions;
}

}  // namespace dnd
