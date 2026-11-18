#include "core/conditions.h"

namespace dnd {

auto DefaultConditions() -> const std::vector<std::string>& {
  static const std::vector<std::string> kConditions{
      "сбит с ног", "отравлен", "испуган",      "захвачен",
      "опутан",     "оглушён",  "без сознания", "концентрация",
  };
  return kConditions;
}

}  // namespace dnd
