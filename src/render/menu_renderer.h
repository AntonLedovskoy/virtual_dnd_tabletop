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
  // Баннер обновления в шапке панели: вся полоса и две кнопки.
  SDL_FRect update_banner;
  SDL_FRect update_action;
  SDL_FRect update_dismiss;
};

// Баннер обновления над вкладками. Пока он виден, подзаголовок меню уступает
// место версии и кнопкам: места в шапке ровно на одну строку.
struct MenuUpdate {
  bool visible = false;
  std::string message;
  std::string action;
  std::string dismiss;
  bool action_enabled = true;
};

auto ComputeMenuLayout(int window_width, int window_height, MenuTab tab)
    -> MenuLayout;

auto DrawMenu(SDL_Renderer* renderer, TextRenderer* text,
              const MenuState& state,
              const std::vector<std::string>& local_addresses,
              const MenuUpdate& update) -> void;

}  // namespace dnd
