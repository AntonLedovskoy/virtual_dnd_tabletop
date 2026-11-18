#pragma once

#include <string>
#include <vector>

namespace dnd {

// Стандартные метки состояний. Это подсказки для стола, а не автоматические
// модификаторы бросков.
[[nodiscard]] auto DefaultConditions() -> const std::vector<std::string>&;

}  // namespace dnd
