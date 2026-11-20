#pragma once

#include <SDL3/SDL.h>

#include <cstdint>
#include <string>
#include <vector>

namespace dnd {

class TextRenderer;

enum class MenuTab : std::uint8_t { kHost, kJoin, kEditor };

enum class MenuField : std::int8_t {
  kNone = -1,
  kAddress = 0,
  kPort = 1,
  kName = 2,
  kPassword = 3,
};

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
};

struct MenuLayout {
  SDL_FRect host_tab;
  SDL_FRect join_tab;
  SDL_FRect editor_tab;
  SDL_FRect address_field;
  SDL_FRect port_field;
  SDL_FRect name_field;
  SDL_FRect password_field;
  SDL_FRect action_button;
  // Строка выбора кампании на вкладке хоста.
  SDL_FRect campaign_line;
};

auto ComputeMenuLayout(int window_width, int window_height, MenuTab tab)
    -> MenuLayout;

auto DrawMenu(SDL_Renderer* renderer, TextRenderer* text,
              const MenuState& state,
              const std::vector<std::string>& local_addresses) -> void;

}  // namespace dnd
