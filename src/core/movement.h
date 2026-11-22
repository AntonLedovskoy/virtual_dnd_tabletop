#pragma once

#include <string_view>

#include "core/grid_map.h"
#include "core/token.h"

namespace dnd {

// «Сбит с ног»: можно ползти или встать за половину скорости.
inline constexpr std::string_view kProne = "сбит с ног";

// В модели нет характеристик, поэтому длинный прыжок с разбега считается
// фиксированным: 10 футов = две клетки. Это осознанное упрощение 2024.
inline constexpr int kMaxJumpCells = 2;

// Падение: 1d6 дробящего урона за каждые 10 футов, максимум 20d6.
inline constexpr int kFallFeetPerDie = 10;
inline constexpr int kMaxFallDice = 20;

// Максимальная высота падения в команде DM: дальше кости всё равно
// ограничены kMaxFallDice, но число должно оставаться разумным.
inline constexpr int kMaxFallFeet = 2000;

[[nodiscard]] auto IsProne(const Token& token) -> bool;

// Сколько клеток движения стоит встать из «сбит с ног»: половина скорости,
// но не меньше одной клетки. Вне боя вставание не тратит запас.
[[nodiscard]] auto StandUpCostCells(const Token& token) -> int;

// Запас движения на ход с учётом рывка.
[[nodiscard]] auto MovementBudgetCells(const Token& token) -> int;

// Остаток движения в бою: бюджет минус потраченное и цена вставания.
// Именно это значение показывает M и проверяет сервер.
[[nodiscard]] auto RemainingMovementCells(const Token& token) -> int;

// Ползание: каждый фут стоит вдвое дороже.
[[nodiscard]] auto CrawlCostCells(int path_cost) -> int;

// Дистанция прыжка в клетках или 0, если цель не на прямой линии.
[[nodiscard]] auto JumpDistanceCells(Cell from, Cell to) -> int;

// Свободна ли линия прыжка от стен. Пропасть, вода и лава прыжку не мешают:
// их можно перепрыгнуть, но приземляться в них нельзя.
[[nodiscard]] auto IsJumpPathClear(const GridMap& map, Cell from, Cell to)
    -> bool;

// Сколько d6 урона даёт падение с указанной высоты.
[[nodiscard]] auto FallDamageDice(int feet) -> int;

}  // namespace dnd
