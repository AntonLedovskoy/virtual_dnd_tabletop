#pragma once

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace dnd {

// Ключи, которые понимает секция одного типа контента. Один парсер на все
// будущие типы: заклинания и предметы добавятся своей схемой, а не вторым
// форматом файла.
struct ContentSchema {
  std::string_view type;
  std::span<const std::string_view> keys;
};

struct ContentEntry {
  std::string key;
  std::string value;
  int line = 0;
};

struct ContentSection {
  std::string type;
  std::string id;
  int line = 0;
  std::vector<ContentEntry> entries;

  [[nodiscard]] auto Find(std::string_view key) const -> const ContentEntry*;
};

struct ContentFile {
  int format = 0;
  std::vector<ContentSection> sections;
};

// Построчный разбор текстового контента: BOM и CRLF допустимы, пустые строки
// и строки с `#` пропускаются, строка `format = 1` обязательна и идёт до
// первой секции, секция — `[тип id]`, поле — `ключ = значение`. Незнакомый
// тип или ключ, повтор поля, обрыв строки и отсутствие format — ошибка с
// номером строки; при ошибке file недействителен.
[[nodiscard]] auto ParseContent(std::string_view text,
                                std::span<const ContentSchema> schemas,
                                ContentFile& file, std::string& error) -> bool;

}  // namespace dnd
