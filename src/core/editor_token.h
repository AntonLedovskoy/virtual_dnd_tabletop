#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "core/grid_map.h"
#include "core/token.h"

namespace dnd {

// Подпись под кругом токена: длиннее не влезает на клетку.
inline constexpr std::size_t kMaxTokenNameLength = 64;

// Клетка в пределах карты, не непроходимая и не занятая чужим токеном.
// moving_id исключает сам перемещаемый токен из проверки.
[[nodiscard]] auto CanPlaceToken(const GridMap& map,
                                 const std::vector<Token>& tokens, Cell cell,
                                 TokenId moving_id = 0) -> bool;

// Переставляет токен, если CanPlaceToken; true — клетка изменилась.
auto MoveTokenToCell(std::vector<Token>& tokens, const GridMap& map, TokenId id,
                     Cell cell) -> bool;

// Имя без пробелов по краям; пустая строка — имя не годится.
[[nodiscard]] auto TrimTokenName(std::string_view name) -> std::string;

}  // namespace dnd
