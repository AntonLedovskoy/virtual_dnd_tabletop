#pragma once

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/bestiary.h"
#include "core/content_parser.h"

namespace dnd {

// Схема единственного пока типа контента. Новые типы добавятся сюда, а
// `core/content_parser` останется общим.
[[nodiscard]] auto MonsterContentSchemas() -> std::span<const ContentSchema>;

// Разбирает уже прочитанный файл и проверяет записи: обязательные поля,
// уникальность `id`, кости, выражение урона и списки защит. При ошибке
// возвращает пустой optional и текст с номером строки.
[[nodiscard]] auto ParseMonsterCatalog(const ContentFile& file,
                                       std::string& error)
    -> std::optional<std::vector<MonsterTemplate>>;

// Один шаг для библиотеки: текст файла → проверенные шаблоны.
[[nodiscard]] auto ParseMonsterContent(std::string_view text,
                                       std::string& error)
    -> std::optional<std::vector<MonsterTemplate>>;

}  // namespace dnd
