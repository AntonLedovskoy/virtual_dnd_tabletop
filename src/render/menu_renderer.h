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
  // Строка партии: продолжение, новая партия или чужая версия кампании.
  SDL_FRect party_line;
  // Баннер обновления в шапке панели: вся полоса и две кнопки.
  SDL_FRect update_banner;
  SDL_FRect update_action;
  SDL_FRect update_dismiss;
  // Модальный диалог несовпадения версий поверх меню.
  SDL_FRect mismatch_panel;
  SDL_FRect mismatch_action;
  SDL_FRect mismatch_close;
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

// Диалог несовпадения версий: появляется, когда хост отверг подключение по
// протоколу, и закрывает меню, пока игрок не обновится или не закроет его.
struct MenuMismatch {
  bool visible = false;
  std::string title;
  // «У вас: …», «У хоста: …», «Последняя: …».
  std::string mine;
  std::string host;
  std::string latest;
  std::string advice;
  // Пусто — обновление не поможет (обновиться должен хост).
  std::string action;
  // true — кнопка открывает страницу релизов, а не запускает обновление.
  bool opens_releases = false;
  std::string close;
};

auto ComputeMenuLayout(int window_width, int window_height, MenuTab tab)
    -> MenuLayout;

auto DrawMenu(SDL_Renderer* renderer, TextRenderer* text,
              const MenuState& state,
              const std::vector<std::string>& local_addresses,
              const MenuUpdate& update, const MenuMismatch& mismatch) -> void;

}  // namespace dnd
