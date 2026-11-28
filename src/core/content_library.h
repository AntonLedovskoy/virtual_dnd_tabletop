#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "core/bestiary.h"

namespace dnd {

// Контент — ассет рядом с .exe: его читают и хост, и клиент, и редактор.
inline constexpr std::string_view kShippedMonsterContent =
    "assets/content/monsters.dndcontent";
inline constexpr std::string_view kLocalMonsterContent =
    "assets/content/local/monsters.dndcontent";

struct ContentLibrary {
  // Встроенный набор, перекрытый записями файлов по `id`.
  std::vector<MonsterTemplate> monsters;
  // Пусто — всё прочитано. Иначе предупреждение для шапки панели `B` и
  // технического журнала: стол работает на встроенном наборе.
  std::string warning;
};

// Фиксированный список файлов, а не обход каталога: лишний или
// переименованный файл не читается, поэтому старый ассет после обновления
// ничего не ломает. Порядок слияния: встроенное → поставка → local.
[[nodiscard]] auto LoadContentLibrary(const std::filesystem::path& base)
    -> ContentLibrary;

}  // namespace dnd
