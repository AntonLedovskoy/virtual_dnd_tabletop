#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace dnd {

enum class MenuTab : std::uint8_t { kHost, kJoin, kEditor };

enum class MenuField : std::int8_t {
  kNone = -1,
  kAddress = 0,
  kPort = 1,
  kName = 2,
  kPassword = 3,
};

// Состояние стартового меню. Живёт в ядре, потому что переход по полям,
// лимиты ввода и разбор адреса проверяются тестами без окна.
struct MenuState {
  MenuTab tab = MenuTab::kHost;
  MenuField active_field = MenuField::kNone;
  std::string address = "127.0.0.1";
  std::string port = "7777";
  std::string name = "player";
  std::string password;
  std::string status;
  bool busy = false;
  // Активная сцена кампании для вкладки хоста.
  std::string scene_name;
  std::size_t scene_count = 0;
  // Выбранный файл кампании: имя и позиция в библиотеке.
  std::string campaign_file;
  std::size_t campaign_index = 0;
  std::size_t campaign_count = 0;
  // Версия сборки и версия протокола для вкладки хоста.
  std::string version;
  int protocol = 0;
};

// Поля вкладки в порядке обхода Tab.
[[nodiscard]] auto MenuFieldOrder(MenuTab tab) -> std::span<const MenuField>;

// Соседнее поле вкладки; direction < 0 — назад (Shift+Tab), иначе вперёд.
[[nodiscard]] auto NextMenuField(MenuTab tab, MenuField current, int direction)
    -> MenuField;

// Соседняя вкладка по стрелкам влево/вправо, по кругу.
[[nodiscard]] auto NextMenuTab(MenuTab tab, int direction) -> MenuTab;

// Предел поля в байтах.
[[nodiscard]] auto MenuFieldLimit(MenuField field) -> std::size_t;

// Буфер активного поля или nullptr, если поле не редактируется на вкладке.
[[nodiscard]] auto ActiveMenuText(MenuState& menu) -> std::string*;

// Порт 1..65535 из строки поля. Ноль, пробелы и хвосты считаются ошибкой.
[[nodiscard]] auto ParsePort(std::string_view text, std::uint16_t& port)
    -> bool;

// Разобранный адрес подключения: «хост» или «хост:порт».
struct HostAddress {
  std::string host;
  std::uint16_t port = 0;
  bool has_port = false;
};

// Порт из адреса берётся, только если он корректный; иначе строка остаётся
// хостом целиком.
[[nodiscard]] auto SplitHostPort(std::string_view text) -> HostAddress;

}  // namespace dnd
