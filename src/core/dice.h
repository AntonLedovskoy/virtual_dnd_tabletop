#pragma once

#include <cstdint>
#include <random>
#include <string>
#include <string_view>
#include <vector>

namespace dnd {

struct DiceExpression {
  int count = 1;
  int sides = 20;
  int modifier = 0;
  bool advantage = false;
  bool disadvantage = false;
};

struct DiceRollResult {
  std::vector<int> rolls;
  int chosen = 0;
  int total = 0;
  std::string breakdown;
};

// Поддерживает d20, 2d6+3, adv d20+5, dis d20-1.
auto ParseDiceExpression(std::string_view text, DiceExpression& expression)
    -> bool;

// Проверяет, может ли текст быть началом корректного выражения. Используется
// для посимвольного ввода урона в листе персонажа.
auto IsDiceExpressionPrefix(std::string_view text) -> bool;

auto RollDiceExpression(const DiceExpression& expression, std::mt19937& rng)
    -> DiceRollResult;

}  // namespace dnd
