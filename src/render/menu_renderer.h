#pragma once

#include <SDL3/SDL.h>

#include <string>
#include <vector>

#include "core/menu_model.h"

namespace dnd {

class TextRenderer;

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
