#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace dnd {

// Автоматическое состояние при нуле HP; его ставит и снимает сервер.
inline constexpr std::string_view kUnconscious = "без сознания";

// Сколько ходов действует состояние, включённое в панели K.
inline constexpr int kDefaultConditionRounds = 10;

// Стандартные метки состояний. Это подсказки для стола, а не автоматические
// модификаторы бросков.
[[nodiscard]] auto DefaultConditions() -> const std::vector<std::string>&;

}  // namespace dnd
