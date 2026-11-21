#pragma once

#include <string>
#include <string_view>

namespace dnd {

// Имена игроков приходят из handshake, чата и /owner, поэтому "Player",
// "player" и "PLAYER" — один и тот же игрок. Регистр складывается для
// латиницы, кириллицы и греческого; прочие письменности сравниваются точно.
[[nodiscard]] auto PlayerNameKey(std::string_view name) -> std::string;

// Одно и то же имя? Сравнение без выделения памяти: по кодовым точкам.
[[nodiscard]] auto SamePlayerName(std::string_view left, std::string_view right)
    -> bool;

}  // namespace dnd
