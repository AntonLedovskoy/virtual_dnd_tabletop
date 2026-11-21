#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace dnd {

// Сколько строк файла сессии держим для просмотра и сколько байт читаем с
// конца: журнал растёт годами, а панель показывает последнюю его часть.
inline constexpr std::size_t kMaxHistoryLines = 2000;
inline constexpr std::size_t kMaxHistoryBytes = std::size_t{512} * 1024;

// Строка файла сессии в том виде, в каком её показывает панель журнала.
// header — заголовок «=== сессия ... ===»; у обычной строки время может быть
// пустым, если его не было в файле.
struct HistoryLine {
  std::string time;
  std::string author;
  std::string text;
  bool header = false;
};

// Разбирает тело session.log. Пустые строки пропускаются, CR в конце строки
// отбрасывается, последние max_lines строк остаются.
[[nodiscard]] auto ParseSessionLog(std::string_view text, std::size_t max_lines)
    -> std::vector<HistoryLine>;

// Читает хвост файла сессии: не больше kMaxHistoryBytes с конца, чтобы не
// грузить весь многолетний журнал. Отсутствующий файл даёт пустой список.
[[nodiscard]] auto LoadSessionLog(const std::string& path,
                                  std::size_t max_lines)
    -> std::vector<HistoryLine>;

// Индексы строк, подходящих под запрос без учёта регистра (тем же ключом,
// что и имена игроков). Заголовок сессии попадает в результат, только если
// нашлась хотя бы одна строка этой сессии. Пустой запрос возвращает всё.
[[nodiscard]] auto SearchSessionLog(const std::vector<HistoryLine>& lines,
                                    std::string_view query)
    -> std::vector<std::size_t>;

}  // namespace dnd
