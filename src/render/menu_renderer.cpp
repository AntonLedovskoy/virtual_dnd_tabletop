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
// Диалог несовпадения версий: шире кнопки, но уже панели меню.
constexpr float kMismatchWidth = 560.0F;
constexpr float kMismatchHeight = 280.0F;
constexpr float kMismatchButtonHeight = 40.0F;

auto SetDrawColor(SDL_Renderer* renderer, SDL_Color color) -> void {
  SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
}

auto PartyText(const MenuState& state) -> std::string {
  std::string text(strings::kPartyLabel);
  switch (state.party_state) {
    case PartyState::kNone:
      text += strings::kPartyNone;
      break;
    case PartyState::kNew:
      text += strings::kPartyNew;
      break;
    case PartyState::kContinued:
      text += strings::kPartyContinued;
      text += std::to_string(state.party_scenes);
      break;
    case PartyState::kMismatch:
      text += strings::kPartyMismatch;
      break;
  }
  return text;
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

// Модальный диалог поверх меню: версии сторон, вердикт и кнопки. Рисуется
// последним, чтобы шапка и поля меню под ним не перекрывали текст.
auto DrawMismatch(SDL_Renderer* renderer, TextRenderer* text,
                  const MenuLayout& layout, const MenuMismatch& mismatch)
    -> void {
  if (text == nullptr || !mismatch.visible) {
    return;
  }
  SetDrawColor(renderer, kBackground);
  SDL_RenderFillRect(renderer, &layout.mismatch_panel);
  SetDrawColor(renderer, kActive);
  SDL_RenderRect(renderer, &layout.mismatch_panel);

  const float left = layout.mismatch_panel.x + 20.0F;
  const float width = layout.mismatch_panel.w - 40.0F;
  float y = layout.mismatch_panel.y + 18.0F;
  text->Draw(mismatch.title, left, y, kText, width);
  y += 40.0F;
  constexpr float line_step = 24.0F;
  text->Draw(mismatch.mine, left, y, kText, width);
  y += line_step;
  text->Draw(mismatch.host, left, y, kText, width);
  y += line_step;
  text->Draw(mismatch.latest, left, y, kHint, width);
  y += line_step + 8.0F;
  text->DrawWrapped(mismatch.advice, left, y, kHint, width);
  text->Draw(std::string(strings::kMismatchHint), left,
             layout.mismatch_action.y - 28.0F, kHint, width);

  SetDrawColor(renderer, kButtonDisabled);
  SDL_RenderFillRect(renderer, &layout.mismatch_close);
  SetDrawColor(renderer, kBorder);
  SDL_RenderRect(renderer, &layout.mismatch_close);
  text->DrawCentered(mismatch.close, layout.mismatch_close, kText);
  if (!mismatch.action.empty()) {
    SetDrawColor(renderer, kButton);
    SDL_RenderFillRect(renderer, &layout.mismatch_action);
    SetDrawColor(renderer, kBorder);
    SDL_RenderRect(renderer, &layout.mismatch_action);
    text->DrawCentered(mismatch.action, layout.mismatch_action, kButtonText);
  }
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
  // Шапка: строка названия и, при обновлении, полоса с кнопками вместо
  // подзаголовка. Раскладка полей ниже не сдвигается.
  layout.update_banner =
      SDL_FRect{field_x, panel_y + 34.0F, field_width, 42.0F};
  constexpr float button_height = 32.0F;
  constexpr float dismiss_width = 80.0F;
  constexpr float action_width = 248.0F;
  constexpr float banner_padding = 6.0F;
  layout.update_dismiss =
      SDL_FRect{field_x + field_width - banner_padding - dismiss_width,
                panel_y + 39.0F, dismiss_width, button_height};
  layout.update_action =
      SDL_FRect{layout.update_dismiss.x - 8.0F - action_width, panel_y + 39.0F,
                action_width, button_height};
  // Диалог версий стоит по центру окна: он модальный и не зависит от вкладки.
  const float mismatch_x =
      (static_cast<float>(window_width) - kMismatchWidth) / 2.0F;
  const float mismatch_y =
      (static_cast<float>(window_height) - kMismatchHeight) / 2.0F;
  layout.mismatch_panel =
      SDL_FRect{mismatch_x, mismatch_y, kMismatchWidth, kMismatchHeight};
  layout.mismatch_action =
      SDL_FRect{mismatch_x + 20.0F,
                mismatch_y + kMismatchHeight - 20.0F - kMismatchButtonHeight,
                280.0F, kMismatchButtonHeight};
  layout.mismatch_close =
      SDL_FRect{mismatch_x + kMismatchWidth - 20.0F - 160.0F,
                mismatch_y + kMismatchHeight - 20.0F - kMismatchButtonHeight,
                160.0F, kMismatchButtonHeight};
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
    layout.party_line =
        SDL_FRect{field_x, panel_y + 414.0F, field_width, 44.0F};
    layout.action_button =
        SDL_FRect{field_x, panel_y + 478.0F, field_width, 50.0F};
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
              const std::vector<std::string>& local_addresses,
              const MenuUpdate& update, const MenuMismatch& mismatch) -> void {
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
  text->Draw(std::string(strings::kMenuTitle), panel_x + 40.0F, panel_y + 8.0F,
             kText, field_width);
  if (state.tab == MenuTab::kHost && !state.version.empty()) {
    const std::string version =
        std::string(strings::kVersionLabel) + state.version + " | " +
        std::string(strings::kProtocolLabel) + std::to_string(state.protocol);
    const TextMetrics metrics = text->Measure(version);
    text->Draw(version, panel_x + kPanelWidth - 40.0F - metrics.width,
               panel_y + 8.0F, kHint);
  }
  if (!update.visible) {
    text->Draw(std::string(strings::kMenuSubtitle), panel_x + 40.0F,
               panel_y + 48.0F, kHint, field_width);
  }

  const MenuLayout layout =
      ComputeMenuLayout(window_width, window_height, state.tab);

  if (update.visible) {
    // Версия и кнопки на месте подзаголовка: шапка не раздвигает поля.
    SetDrawColor(renderer, kPanel);
    SDL_RenderFillRect(renderer, &layout.update_banner);
    SetDrawColor(renderer, kActive);
    SDL_RenderRect(renderer, &layout.update_banner);
    const float text_width =
        layout.update_action.x - (layout.update_banner.x + 12.0F) - 10.0F;
    const TextMetrics metrics = text->Measure(update.message);
    text->Draw(update.message, layout.update_banner.x + 12.0F,
               layout.update_banner.y +
                   ((layout.update_banner.h - metrics.height) / 2.0F),
               kText, text_width);
    SetDrawColor(renderer, update.action_enabled ? kButton : kButtonDisabled);
    SDL_RenderFillRect(renderer, &layout.update_action);
    SetDrawColor(renderer, kBorder);
    SDL_RenderRect(renderer, &layout.update_action);
    SDL_RenderRect(renderer, &layout.update_dismiss);
    text->DrawCentered(update.action, layout.update_action, kButtonText);
    text->DrawCentered(update.dismiss, layout.update_dismiss, kText);
  }

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
    const bool mismatch = state.party_state == PartyState::kMismatch;
    text->Draw(PartyText(state), layout.party_line.x, layout.party_line.y,
               mismatch ? kError : kText, field_width);
    text->Draw(std::string(mismatch ? strings::kPartyMismatchHint
                                    : strings::kPartyHint),
               layout.party_line.x, layout.party_line.y + 22.0F, kHint,
               field_width);
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

  DrawMismatch(renderer, text, layout, mismatch);
}

}  // namespace dnd
