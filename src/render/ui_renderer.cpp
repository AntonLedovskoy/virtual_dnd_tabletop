#include "render/ui_renderer.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <string>

#include "app/strings.h"
#include "render/text_renderer.h"

namespace dnd {
namespace {

constexpr SDL_Color kPanelBackground{
    .r = 0x0c, .g = 0x0c, .b = 0x12, .a = 0xd0};
constexpr SDL_Color kPanelBorder{
    .r = 0x3a, .g = 0x3a, .b = 0x48, .a = SDL_ALPHA_OPAQUE};
constexpr SDL_Color kLogText{
    .r = 0xdc, .g = 0xdc, .b = 0xe4, .a = SDL_ALPHA_OPAQUE};
constexpr SDL_Color kAuthorText{
    .r = 0x9c, .g = 0xc8, .b = 0xff, .a = SDL_ALPHA_OPAQUE};
constexpr SDL_Color kInputText{
    .r = 0xff, .g = 0xff, .b = 0xff, .a = SDL_ALPHA_OPAQUE};
constexpr SDL_Color kHintText{
    .r = 0x80, .g = 0x80, .b = 0x90, .a = SDL_ALPHA_OPAQUE};

constexpr int kPadding = 12;
constexpr int kVisibleLines = 7;
constexpr int kLineSpacing = 4;

auto SetDrawColor(SDL_Renderer* renderer, SDL_Color color) -> void {
  SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
}

auto KindLabel(TokenKind kind) -> std::string_view {
  switch (kind) {
    case TokenKind::kPlayer:
      return strings::kPlayer;
    case TokenKind::kNpc:
      return strings::kNpc;
    case TokenKind::kMonster:
      return strings::kMonster;
  }
  return strings::kMonster;
}

auto FormatLogTime(std::uint32_t timestamp) -> std::string {
  if (timestamp == 0) {
    return {};
  }
  const std::time_t time = static_cast<std::time_t>(timestamp);
  const std::tm* local = std::localtime(&time);
  if (local == nullptr) {
    return {};
  }
  char buffer[16] = {};
  if (std::strftime(buffer, sizeof(buffer), "%H:%M:%S", local) == 0) {
    return {};
  }
  return buffer;
}

}  // namespace

auto DrawLogPanel(SDL_Renderer* renderer, TextRenderer* text,
                  const std::vector<LogLine>& lines, bool typing,
                  const std::string& input) -> void {
  int window_width = 0;
  int window_height = 0;
  if (!SDL_GetRenderOutputSize(renderer, &window_width, &window_height)) {
    return;
  }
  if (window_width <= 0 || window_height <= 0) {
    return;
  }

  const auto max_text_width =
      static_cast<float>(window_width - (2 * kPadding)) - 160.0F;
  const int line_height =
      text != nullptr ? static_cast<int>(text->LineHeight()) : 16;
  const std::string hint_text = std::string(strings::kChatHint);
  int hint_line_count = 1;
  if (!typing && text != nullptr) {
    const float hint_height = text->Measure(hint_text, max_text_width).height;
    const float row_height = std::max(1.0F, text->LineHeight() + kLineSpacing);
    hint_line_count =
        std::max(1, static_cast<int>(std::ceil(hint_height / row_height)));
  }
  const int content_height = (kVisibleLines * (line_height + kLineSpacing)) +
                             (hint_line_count * (line_height + kLineSpacing));
  const int panel_height = content_height + (2 * kPadding);
  const auto panel_top = static_cast<float>(window_height - panel_height);

  SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
  const SDL_FRect panel{
      .x = 0.0F,
      .y = panel_top,
      .w = static_cast<float>(window_width),
      .h = static_cast<float>(panel_height),
  };
  SetDrawColor(renderer, kPanelBackground);
  SDL_RenderFillRect(renderer, &panel);
  SetDrawColor(renderer, kPanelBorder);
  SDL_RenderLine(renderer, 0.0F, panel_top, static_cast<float>(window_width),
                 panel_top);
  SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);

  const int first_line =
      std::max<int>(0, static_cast<int>(lines.size()) - kVisibleLines);
  auto y = panel_top + static_cast<float>(kPadding);
  for (auto i = static_cast<std::size_t>(first_line); i < lines.size(); ++i) {
    const LogLine& line = lines[i];
    const auto left = static_cast<float>(kPadding);
    auto x = left;
    const std::string time = FormatLogTime(line.timestamp);
    if (!time.empty() && text != nullptr) {
      const std::string prefix = "[" + time + "] ";
      text->Draw(prefix, x, y, kHintText, max_text_width);
      x += text->Measure(prefix).width;
    }
    if (!line.author.empty() && text != nullptr) {
      const std::string author = line.author + ":";
      const float available = max_text_width - (x - left);
      if (available > 0.0F) {
        text->Draw(author, x, y, kAuthorText, available);
      }
      x += text->Measure(author + " ").width;
    }
    const float text_width = max_text_width - (x - left);
    if (text != nullptr && text_width > 0.0F) {
      text->Draw(line.text, x, y, kLogText, text_width);
    }
    y += static_cast<float>(line_height + kLineSpacing);
  }

  const float input_y =
      panel_top + static_cast<float>(kPadding) +
      static_cast<float>(kVisibleLines * (line_height + kLineSpacing));
  if (text != nullptr) {
    if (typing) {
      text->Draw("> " + input + "_", static_cast<float>(kPadding), input_y,
                 kInputText, max_text_width);
    } else {
      text->DrawWrapped(hint_text, static_cast<float>(kPadding), input_y,
                        kHintText, max_text_width);
    }
  }
}

auto DrawInitiativePanel(SDL_Renderer* renderer, TextRenderer* text,
                         const std::vector<Token>& tokens,
                         const Encounter& encounter) -> void {
  if (text == nullptr || tokens.empty()) {
    return;
  }
  int window_width = 0;
  int window_height = 0;
  if (!SDL_GetRenderOutputSize(renderer, &window_width, &window_height)) {
    return;
  }
  if (window_width <= 0 || window_height <= 0) {
    return;
  }

  std::vector<const Token*> sorted;
  sorted.reserve(tokens.size());
  for (const Token& token : tokens) {
    sorted.push_back(&token);
  }
  std::ranges::stable_sort(sorted,
                           [](const Token* left, const Token* right) -> bool {
                             if (left->initiative != right->initiative) {
                               return left->initiative > right->initiative;
                             }
                             return left->name < right->name;
                           });

  constexpr int panel_width = 230;
  constexpr int panel_padding = 10;
  constexpr int max_rows = 10;
  const int line_height = static_cast<int>(text->LineHeight()) + 4;
  const int rows = std::min<int>(max_rows, static_cast<int>(sorted.size()));
  const int panel_height = ((rows + 1) * line_height) + (2 * panel_padding);
  const auto panel_x = static_cast<float>(window_width - panel_width - 12);

  SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
  const SDL_FRect panel{
      .x = panel_x,
      .y = 12.0F,
      .w = static_cast<float>(panel_width),
      .h = static_cast<float>(panel_height),
  };
  SetDrawColor(renderer, kPanelBackground);
  SDL_RenderFillRect(renderer, &panel);
  SetDrawColor(renderer, kPanelBorder);
  SDL_RenderRect(renderer, &panel);
  SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);

  const auto text_x = panel_x + static_cast<float>(panel_padding);
  const auto max_text_width =
      static_cast<float>(panel_width - (2 * panel_padding));
  auto y = 12.0F + static_cast<float>(panel_padding);
  const std::string title = encounter.active
                                ? std::string(strings::kInitiative) + " — " +
                                      std::string(strings::kRound) + " " +
                                      std::to_string(encounter.round)
                                : std::string(strings::kInitiative);
  text->Draw(title, text_x, y, kAuthorText, max_text_width);
  y += static_cast<float>(line_height);

  for (int i = 0; i < rows; ++i) {
    const Token& token = *sorted[static_cast<std::size_t>(i)];
    const bool current = encounter.active &&
                         encounter.turn_index < encounter.order.size() &&
                         encounter.order[encounter.turn_index] == token.id;
    const std::string line = (current ? "> " : "  ") +
                             std::to_string(token.initiative) + "  " +
                             token.name + "  " + std::to_string(token.hp) +
                             "/" + std::to_string(token.max_hp);
    text->Draw(line, text_x, y, current ? kAuthorText : kLogText,
               max_text_width);
    y += static_cast<float>(line_height);
  }
}

auto DrawTokenPanel(SDL_Renderer* renderer, TextRenderer* text,
                    const Token* token, const std::string& player_name,
                    bool is_client) -> float {
  if (renderer == nullptr || text == nullptr || token == nullptr) {
    return 0.0F;
  }
  int window_width = 0;
  int window_height = 0;
  if (!SDL_GetRenderOutputSize(renderer, &window_width, &window_height)) {
    return 0.0F;
  }
  if (window_width <= 0 || window_height <= 0) {
    return 0.0F;
  }

  const std::string owner =
      token->owner.empty() ? std::string(strings::kFree) : token->owner;
  std::vector<std::string> lines;
  lines.emplace_back(std::string(strings::kName) + ": " + token->name);
  lines.emplace_back(std::string(strings::kKind) + ": " +
                     std::string(KindLabel(token->kind)));
  lines.emplace_back(std::string(strings::kOwner) + ": " + owner);
  lines.emplace_back(std::string(strings::kHp) + ": " +
                     std::to_string(token->hp) + "/" +
                     std::to_string(token->max_hp) + "   " +
                     std::string(strings::kInitiativeShort) + " " +
                     std::to_string(token->initiative));

  std::vector<std::string> hints;
  if (is_client && token->kind == TokenKind::kPlayer && token->owner.empty()) {
    hints.emplace_back(strings::kClaimHint);
  }
  if (is_client && token->owner == player_name) {
    hints.emplace_back(strings::kYourToken);
  }
  if (!is_client) {
    hints.emplace_back(strings::kDmSpawnHint);
    hints.emplace_back(strings::kDmCommandHint);
  }
  hints.emplace_back(strings::kColorHint);

  constexpr int panel_width = 300;
  constexpr int panel_padding = 10;
  constexpr float hint_width =
      static_cast<float>(panel_width - (2 * panel_padding));
  const float text_line_height = std::max(1.0F, text->LineHeight());
  int hint_row_count = 0;
  for (const std::string& hint : hints) {
    const float hint_height = text->Measure(hint, hint_width).height;
    hint_row_count += std::max(
        1, static_cast<int>(std::ceil(hint_height / text_line_height)));
  }

  const int line_height = static_cast<int>(text->LineHeight()) + 4;
  const auto row_count = static_cast<int>(1 + lines.size() + hint_row_count);
  const int panel_height = (row_count * line_height) + (2 * panel_padding);

  SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
  const SDL_FRect panel{
      .x = 12.0F,
      .y = 12.0F,
      .w = static_cast<float>(panel_width),
      .h = static_cast<float>(panel_height),
  };
  SetDrawColor(renderer, kPanelBackground);
  SDL_RenderFillRect(renderer, &panel);
  SetDrawColor(renderer, kPanelBorder);
  SDL_RenderRect(renderer, &panel);
  SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);

  const auto text_x = 12.0F + static_cast<float>(panel_padding);
  const auto max_text_width =
      static_cast<float>(panel_width - (2 * panel_padding));
  auto y = 12.0F + static_cast<float>(panel_padding);
  text->Draw(std::string(strings::kToken), text_x, y, kAuthorText,
             max_text_width);
  y += static_cast<float>(line_height);
  for (const std::string& line : lines) {
    text->Draw(line, text_x, y, kLogText, max_text_width);
    y += static_cast<float>(line_height);
  }
  for (const std::string& hint : hints) {
    y = text->DrawWrapped(hint, text_x, y, kHintText, max_text_width) + 4.0F;
  }
  return panel.y + panel.h;
}

auto DrawPlayersPanel(SDL_Renderer* renderer, TextRenderer* text,
                      const std::vector<std::string>& players, float panel_y)
    -> void {
  if (renderer == nullptr || text == nullptr || players.empty()) {
    return;
  }
  constexpr int panel_width = 220;
  constexpr int panel_x = 12;
  constexpr int panel_padding = 10;
  constexpr int max_rows = 8;

  const int line_height = static_cast<int>(text->LineHeight()) + 4;
  const int rows = std::min<int>(max_rows, static_cast<int>(players.size()));
  const int panel_height = ((rows + 1) * line_height) + (2 * panel_padding);

  SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
  const SDL_FRect panel{
      .x = static_cast<float>(panel_x),
      .y = static_cast<float>(panel_y),
      .w = static_cast<float>(panel_width),
      .h = static_cast<float>(panel_height),
  };
  SetDrawColor(renderer, kPanelBackground);
  SDL_RenderFillRect(renderer, &panel);
  SetDrawColor(renderer, kPanelBorder);
  SDL_RenderRect(renderer, &panel);
  SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);

  const auto text_x = static_cast<float>(panel_x + panel_padding);
  const auto max_text_width =
      static_cast<float>(panel_width - (2 * panel_padding));
  auto y = static_cast<float>(panel_y + panel_padding);
  text->Draw(std::string(strings::kPlayers), text_x, y, kAuthorText,
             max_text_width);
  y += static_cast<float>(line_height);
  for (int i = 0; i < rows; ++i) {
    text->Draw(players[static_cast<std::size_t>(i)], text_x, y, kLogText,
               max_text_width);
    y += static_cast<float>(line_height);
  }
}

}  // namespace dnd
