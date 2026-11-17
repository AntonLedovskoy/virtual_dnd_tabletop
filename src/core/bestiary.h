#pragma once

#include <string>
#include <vector>

namespace dnd {

// Небольшая встроенная библиотека монстров. При спавне шаблон копируется в
// токен, поэтому редактирование монстра не меняет шаблон.
struct MonsterTemplate {
  std::string name;
  int ac = 10;
  int max_hp = 10;
  int speed = 30;
  std::string notes;
};

[[nodiscard]] auto DefaultBestiary() -> const std::vector<MonsterTemplate>&;

}  // namespace dnd
