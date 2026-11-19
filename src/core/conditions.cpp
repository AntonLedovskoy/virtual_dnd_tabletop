#include "core/conditions.h"

namespace dnd {

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
      "концентрация",
  };
  return kConditions;
}

}  // namespace dnd
