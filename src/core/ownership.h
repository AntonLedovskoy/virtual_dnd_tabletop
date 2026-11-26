#pragma once

#include "core/token.h"

namespace dnd {

// Персонаж, которого ведёт сам DM: владелец — "dm". Монстры и NPC тоже
// принадлежат DM, но персонажами не считаются.
[[nodiscard]] auto IsDmOwnedCharacter(const Token& token) -> bool;

// Токен играет по правилам игрока: очередь, недееспособность, скорость 0,
// занятая клетка, путь, бюджет движения, действие и реакция. Для игрока это
// все его токены, для DM — только взятые им персонажи. Монстры, NPC и ещё не
// взятые персонажи остаются мастерскими: DM двигает их свободно.
[[nodiscard]] auto FollowsPlayerRules(const Token& token, bool can_control_all)
    -> bool;

}  // namespace dnd
