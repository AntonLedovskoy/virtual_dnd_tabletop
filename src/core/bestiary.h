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
  int speed = 9;
  int attack_bonus = 0;
  int save_bonus = 0;
  std::string damage;
  std::string notes;
};

[[nodiscard]] auto DefaultBestiary() -> const std::vector<MonsterTemplate>&;

}  // namespace dnd
