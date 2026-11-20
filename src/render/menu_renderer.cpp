#include "render/menu_renderer.h"

#include <algorithm>
#include <string>

#include "app/strings.h"
#include "render/text_renderer.h"
#include "render/ui_renderer.h"

namespace dnd {
namespace {

constexpr SDL_Color kBackground{
    .r = 0x18, .g = 0x18, .b = 0x20, .a = SDL_ALPHA_OPAQUE};
constexpr SDL_Color kPanel{
    .r = 0x0c, .g = 0x0c, .b = 0x12, .a = SDL_ALPHA_OPAQUE};
constexpr SDL_Color kBorder{
    .r = 0x3a, .g = 0x3a, .b = 0x48, .a = SDL_ALPHA_OPAQUE};
constexpr SDL_Color kActive{
    .r = 0x6c, .g = 0xa8, .b = 0xff, .a = SDL_ALPHA_OPAQUE};
constexpr SDL_Color kText{
    .r = 0xdc, .g = 0xdc, .b = 0xe4, .a = SDL_ALPHA_OPAQUE};
constexpr SDL_Color kHint{
    .r = 0x80, .g = 0x80, .b = 0x90, .a = SDL_ALPHA_OPAQUE};
constexpr SDL_Color kButton{
    .r = 0x50, .g = 0x7a, .b = 0xc8, .a = SDL_ALPHA_OPAQUE};
constexpr SDL_Color kButtonDisabled{
    .r = 0x32, .g = 0x32, .b = 0x40, .a = SDL_ALPHA_OPAQUE};
constexpr SDL_Color kButtonText{
    .r = 0xff, .g = 0xff, .b = 0xff, .a = SDL_ALPHA_OPAQUE};
constexpr SDL_Color kError{
    .r = 0xff, .g = 0x90, .b = 0x90, .a = SDL_ALPHA_OPAQUE};

constexpr float kPanelWidth = 640.0F;
constexpr float kPanelHeight = 660.0F;
constexpr float kFieldHeight = 44.0F;

auto SetDrawColor(SDL_Renderer* renderer, SDL_Color color) -> void {
  SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
}

auto DrawField(SDL_Renderer* renderer, TextRenderer* text,
               const SDL_FRect& rect, const std::string& label,
               const std::string& value, bool active, bool password) -> void {
  if (text == nullptr || rect.w <= 0.0F || rect.h <= 0.0F) {
    return;
  }
  text->Draw(label, rect.x, rect.y - 24.0F, active ? kActive : kHint, rect.w);

  SetDrawColor(renderer, kPanel);
  SDL_RenderFillRect(renderer, &rect);
  SetDrawColor(renderer, active ? kActive : kBorder);
  SDL_RenderRect(renderer, &rect);

  std::string shown = password ? std::string(value.size(), '*') : value;
  if (shown.empty()) {
    shown = " ";
  }
  const float text_y = rect.y + ((rect.h - text->LineHeight()) / 2.0F);
  text->Draw(shown, rect.x + 12.0F, text_y, value.empty() ? kHint : kText,
             rect.w - 24.0F);
}

}  // namespace

auto ComputeMenuLayout(int window_width, int window_height, MenuTab tab)
    -> MenuLayout {
  const float panel_x = (static_cast<float>(window_width) - kPanelWidth) / 2.0F;
  const float panel_y =
      (static_cast<float>(window_height) - kPanelHeight) / 2.0F;
  const float field_x = panel_x + 40.0F;
  const float field_width = kPanelWidth - 80.0F;

  MenuLayout layout{};
  layout.host_tab = SDL_FRect{panel_x + 30.0F, panel_y + 80.0F, 180.0F, 44.0F};
  layout.join_tab = SDL_FRect{panel_x + 230.0F, panel_y + 80.0F, 180.0F, 44.0F};
  layout.editor_tab =
      SDL_FRect{panel_x + 430.0F, panel_y + 80.0F, 180.0F, 44.0F};
  if (tab == MenuTab::kEditor) {
    layout.action_button =
        SDL_FRect{field_x, panel_y + 200.0F, field_width, 50.0F};
  } else if (tab == MenuTab::kHost) {
    layout.port_field =
        SDL_FRect{field_x, panel_y + 190.0F, field_width, kFieldHeight};
    layout.password_field =
        SDL_FRect{field_x, panel_y + 290.0F, field_width, kFieldHeight};
    layout.campaign_line =
        SDL_FRect{field_x, panel_y + 344.0F, field_width, 24.0F};
    layout.action_button =
        SDL_FRect{field_x, panel_y + 430.0F, field_width, 50.0F};
  } else {
    layout.address_field =
        SDL_FRect{field_x, panel_y + 160.0F, field_width, kFieldHeight};
    layout.port_field =
        SDL_FRect{field_x, panel_y + 240.0F, field_width, kFieldHeight};
    layout.name_field =
        SDL_FRect{field_x, panel_y + 320.0F, field_width, kFieldHeight};
    layout.password_field =
        SDL_FRect{field_x, panel_y + 400.0F, field_width, kFieldHeight};
    layout.action_button =
        SDL_FRect{field_x, panel_y + 490.0F, field_width, 50.0F};
  }
  return layout;
}

auto DrawMenu(SDL_Renderer* renderer, TextRenderer* text,
              const MenuState& state,
              const std::vector<std::string>& local_addresses) -> void {
  int window_width = 0;
  int window_height = 0;
  if (text == nullptr || !RenderSize(renderer, window_width, window_height)) {
    return;
  }
  SetDrawColor(renderer, kBackground);
  SDL_RenderClear(renderer);

  const auto panel_x = (static_cast<float>(window_width) - kPanelWidth) / 2.0F;
  const auto panel_y =
      (static_cast<float>(window_height) - kPanelHeight) / 2.0F;
  const SDL_FRect panel{panel_x, panel_y, kPanelWidth, kPanelHeight};
  SetDrawColor(renderer, kPanel);
  SDL_RenderFillRect(renderer, &panel);
  SetDrawColor(renderer, kBorder);
  SDL_RenderRect(renderer, &panel);

  const auto field_width = kPanelWidth - 80.0F;
  text->Draw(std::string(strings::kMenuTitle), panel_x + 40.0F, panel_y + 20.0F,
             kText, field_width);
  text->Draw(std::string(strings::kMenuSubtitle), panel_x + 40.0F,
             panel_y + 48.0F, kHint, field_width);

  const MenuLayout layout =
      ComputeMenuLayout(window_width, window_height, state.tab);

  SetDrawColor(renderer, state.tab == MenuTab::kHost ? kActive : kPanel);
  SDL_RenderFillRect(renderer, &layout.host_tab);
  SetDrawColor(renderer, state.tab == MenuTab::kJoin ? kActive : kPanel);
  SDL_RenderFillRect(renderer, &layout.join_tab);
  SetDrawColor(renderer, state.tab == MenuTab::kEditor ? kActive : kPanel);
  SDL_RenderFillRect(renderer, &layout.editor_tab);
  SetDrawColor(renderer, kBorder);
  SDL_RenderRect(renderer, &layout.host_tab);
  SDL_RenderRect(renderer, &layout.join_tab);
  SDL_RenderRect(renderer, &layout.editor_tab);
  text->DrawCentered(std::string(strings::kHostTab), layout.host_tab,
                     state.tab == MenuTab::kHost ? kButtonText : kText);
  text->DrawCentered(std::string(strings::kJoinTab), layout.join_tab,
                     state.tab == MenuTab::kJoin ? kButtonText : kText);
  text->DrawCentered(std::string(strings::kEditorTab), layout.editor_tab,
                     state.tab == MenuTab::kEditor ? kButtonText : kText);

  if (state.tab == MenuTab::kHost) {
    DrawField(renderer, text, layout.port_field, std::string(strings::kPort),
              state.port, state.active_field == MenuField::kPort, false);
    DrawField(renderer, text, layout.password_field,
              std::string(strings::kPasswordOptional), state.password,
              state.active_field == MenuField::kPassword, true);
    // Что именно начнётся по кнопке: файл кампании, активная сцена и размер.
    const std::string campaign =
        state.campaign_file.empty() ? "—" : state.campaign_file;
    text->Draw(
        std::string(strings::kCampaignFile) + campaign + " (" +
            std::to_string(
                state.campaign_count == 0 ? 0 : state.campaign_index + 1) +
            "/" + std::to_string(state.campaign_count) + ")",
        layout.campaign_line.x, layout.campaign_line.y, kText, field_width);
    const std::string scene = state.scene_name.empty() ? "—" : state.scene_name;
    text->Draw(std::string(strings::kCampaignScene) + scene + " | " +
                   std::string(strings::kCampaignSceneCount) +
                   std::to_string(state.scene_count),
               panel_x + 40.0F, layout.campaign_line.y + 22.0F, kText,
               field_width);
    text->Draw(std::string(strings::kCampaignHint), panel_x + 40.0F,
               layout.campaign_line.y + 46.0F, kHint, field_width);
  } else if (state.tab == MenuTab::kJoin) {
    DrawField(renderer, text, layout.address_field,
              std::string(strings::kAddress), state.address,
              state.active_field == MenuField::kAddress, false);
    DrawField(renderer, text, layout.port_field, std::string(strings::kPort),
              state.port, state.active_field == MenuField::kPort, false);
    DrawField(renderer, text, layout.name_field,
              std::string(strings::kPlayerName), state.name,
              state.active_field == MenuField::kName, false);
    DrawField(renderer, text, layout.password_field,
              std::string(strings::kPassword), state.password,
              state.active_field == MenuField::kPassword, true);
  } else {
    text->DrawWrapped(std::string(strings::kEditorDescription), panel_x + 40.0F,
                      panel_y + 160.0F, kText, field_width);
  }

  SetDrawColor(renderer, state.busy ? kButtonDisabled : kButton);
  SDL_RenderFillRect(renderer, &layout.action_button);
  SetDrawColor(renderer, kBorder);
  SDL_RenderRect(renderer, &layout.action_button);
  std::string action = std::string(strings::kConnect);
  if (state.tab == MenuTab::kHost) {
    action = std::string(strings::kStartHost);
  } else if (state.tab == MenuTab::kEditor) {
    action = std::string(strings::kOpenEditor);
  }
  text->DrawCentered(state.busy ? std::string(strings::kConnecting) : action,
                     layout.action_button, kButtonText);

  auto status_y = layout.action_button.y + layout.action_button.h + 18.0F;
  if (state.tab == MenuTab::kHost) {
    text->Draw(std::string(strings::kHostAddress), panel_x + 40.0F, status_y,
               kHint, field_width);
    status_y += 24.0F;
    if (local_addresses.empty()) {
      text->Draw(std::string(strings::kNoAddress), panel_x + 40.0F, status_y,
                 kText, field_width);
      status_y += 24.0F;
    } else {
      for (std::size_t i = 0; i < local_addresses.size() && i < 2; ++i) {
        text->Draw(local_addresses[i] + ":" + state.port, panel_x + 40.0F,
                   status_y, kText, field_width);
        status_y += 24.0F;
      }
    }
    status_y =
        text->DrawWrapped(std::string(strings::kFirewallHint), panel_x + 40.0F,
                          status_y, kHint, field_width) +
        6.0F;
  }

  const std::string status =
      state.busy ? std::string(strings::kConnecting) : state.status;
  const bool is_error = !state.busy && !state.status.empty();
  text->DrawWrapped(status, panel_x + 40.0F, status_y,
                    is_error ? kError : kHint, field_width);

  text->Draw(std::string(strings::kTabHint), panel_x + 40.0F,
             panel_y + kPanelHeight - 32.0F, kHint, field_width);
}

}  // namespace dnd
