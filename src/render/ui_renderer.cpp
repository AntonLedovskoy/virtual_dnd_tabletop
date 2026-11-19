#include "render/ui_renderer.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <ranges>
#include <span>
#include <string>

#include "app/strings.h"
#include "core/conditions.h"
#include "core/units.h"
#include "render/text_renderer.h"

namespace dnd {
namespace {

constexpr SDL_Color kPanelBackground{
    .r = 0x0c, .g = 0x0c, .b = 0x12, .a = 0xd0};
constexpr SDL_Color kModalBackground{
    .r = 0x0c, .g = 0x0c, .b = 0x12, .a = SDL_ALPHA_OPAQUE};
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
constexpr std::size_t kVisibleSceneRows = 9;
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
  const auto time = static_cast<std::time_t>(timestamp);
  const std::tm* local = std::localtime(&time);
  if (local == nullptr) {
    return {};
  }
  std::array<char, 16> buffer{};
  if (std::strftime(buffer.data(), buffer.size(), "%H:%M:%S", local) == 0) {
    return {};
  }
  return buffer.data();
}

}  // namespace

auto DrawLogPanel(SDL_Renderer* renderer, TextRenderer* text,
                  const std::vector<LogLine>& lines, bool typing,
                  const std::string& input, bool is_dm, int scroll) -> float {
  int window_width = 0;
  int window_height = 0;
  if (!SDL_GetRenderOutputSize(renderer, &window_width, &window_height)) {
    return 0.0F;
  }
  if (window_width <= 0 || window_height <= 0) {
    return 0.0F;
  }

  const auto max_text_width =
      static_cast<float>(window_width - (2 * kPadding)) - 160.0F;
  const int line_height =
      text != nullptr ? static_cast<int>(text->LineHeight()) : 16;
  const std::string hint_text =
      std::string(is_dm ? strings::kChatHintDm : strings::kChatHintPlayer);
  int hint_line_count = 1;
  if (!typing && text != nullptr) {
    const float hint_height = text->Measure(hint_text, max_text_width).height;
    const float row_height = std::max(1.0F, text->LineHeight() + kLineSpacing);
    hint_line_count =
        std::max(1, static_cast<int>(std::ceil(hint_height / row_height)));
  }
  const int content_height = (kLogVisibleLines * (line_height + kLineSpacing)) +
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

  const int total = static_cast<int>(lines.size());
  const int max_scroll = std::max(0, total - kLogVisibleLines);
  const int offset = std::clamp(scroll, 0, max_scroll);
  const int first_line = std::max(0, total - kLogVisibleLines - offset);
  const int last_line = std::min(total, first_line + kLogVisibleLines);
  auto y = panel_top + static_cast<float>(kPadding);
  for (int i = first_line; i < last_line; ++i) {
    const LogLine& line = lines[static_cast<std::size_t>(i)];
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
      static_cast<float>(kLogVisibleLines * (line_height + kLineSpacing));
  if (text != nullptr) {
    if (typing) {
      text->Draw("> " + input + "_", static_cast<float>(kPadding), input_y,
                 kInputText, max_text_width);
    } else if (offset > 0) {
      const std::string scrolled = std::string(strings::kLogScrolledPrefix) +
                                   std::to_string(offset) +
                                   std::string(strings::kLogScrolledSuffix);
      text->Draw(scrolled, static_cast<float>(kPadding), input_y, kHintText,
                 max_text_width);
    } else {
      text->DrawWrapped(hint_text, static_cast<float>(kPadding), input_y,
                        kHintText, max_text_width);
    }
  }
  return panel_top;
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
                    bool is_client, bool can_claim, bool can_release,
                    MovementInfo movement) -> float {
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
  std::string hp_line = std::string(strings::kHp) + ": " +
                        std::to_string(token->hp) + "/" +
                        std::to_string(token->max_hp);
  if (token->temp_hp > 0) {
    hp_line += " (+" + std::to_string(token->temp_hp) + ")";
  }
  hp_line += "   " + std::string(strings::kInitiativeShort) + " " +
             std::to_string(token->initiative);
  lines.push_back(std::move(hp_line));
  lines.emplace_back(std::string(strings::kSheetAc) + ": " +
                     std::to_string(token->ac) + "   " +
                     std::string(strings::kSheetSpeed) + ": " +
                     std::to_string(token->speed));
  if (movement == MovementInfo::kBudget) {
    const int budget = SpeedToCells(token->speed);
    const int left = std::max(0, budget - token->movement_used);
    lines.emplace_back(
        std::string(strings::kMovementLeft) + ": " + FormatMeters(left) + " " +
        std::string(strings::kMovementOf) + " " + FormatMeters(budget));
  } else if (movement == MovementInfo::kFree) {
    lines.emplace_back(strings::kMovementFree);
  }
  if (!token->conditions.empty()) {
    std::string condition_line = std::string(strings::kConditions) + ": ";
    for (std::size_t i = 0; i < token->conditions.size(); ++i) {
      if (i > 0) {
        condition_line += ", ";
      }
      condition_line += token->conditions[i].name;
      if (token->conditions[i].rounds > 0) {
        condition_line +=
            " (" + std::to_string(token->conditions[i].rounds) + ")";
      }
    }
    lines.push_back(std::move(condition_line));
  }

  std::vector<std::string> hints;
  if (is_client && can_claim) {
    hints.emplace_back(strings::kClaimHint);
  }
  if (is_client && can_release) {
    hints.emplace_back(strings::kReleaseHint);
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
  constexpr auto hint_width =
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
      .y = panel_y,
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

namespace {

auto DrawModalField(SDL_Renderer* renderer, TextRenderer* text,
                    const SDL_FRect& rect, const std::string& label,
                    const std::string& value, bool active) -> void {
  const SDL_Color border =
      active ? SDL_Color{.r = 0x6c, .g = 0xa8, .b = 0xff, .a = SDL_ALPHA_OPAQUE}
             : kPanelBorder;
  text->Draw(label, rect.x, rect.y - 24.0F, active ? border : kHintText,
             rect.w);
  SetDrawColor(renderer, kModalBackground);
  SDL_RenderFillRect(renderer, &rect);
  SetDrawColor(renderer, border);
  SDL_RenderRect(renderer, &rect);
  const float text_y = rect.y + ((rect.h - text->LineHeight()) / 2.0F);
  text->Draw(value, rect.x + 10.0F, text_y, kLogText, rect.w - 20.0F);
}

}  // namespace

auto DrawSheetPanel(SDL_Renderer* renderer, TextRenderer* text,
                    const Token& token, SheetField active,
                    const std::string& ac, const std::string& speed,
                    const std::string& attack, const std::string& save,
                    const std::string& initiative, const std::string& temp_hp,
                    const std::string& damage, const std::string& notes)
    -> void {
  if (renderer == nullptr || text == nullptr) {
    return;
  }
  int window_width = 0;
  int window_height = 0;
  if (!SDL_GetRenderOutputSize(renderer, &window_width, &window_height)) {
    return;
  }
  constexpr float width = 640.0F;
  constexpr float height = 520.0F;
  const float x = (static_cast<float>(window_width) - width) / 2.0F;
  const float y = (static_cast<float>(window_height) - height) / 2.0F;
  const SDL_FRect panel{x, y, width, height};
  SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
  SetDrawColor(renderer, kModalBackground);
  SDL_RenderFillRect(renderer, &panel);
  SetDrawColor(renderer, kPanelBorder);
  SDL_RenderRect(renderer, &panel);
  SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);

  const auto left = x + 40.0F;
  const auto content_width = width - 80.0F;
  auto cursor_y = y + 18.0F;
  text->Draw(std::string(strings::kSheetTitle) + ": " + token.name, left,
             cursor_y, kAuthorText, content_width);
  cursor_y += text->LineHeight() + 8.0F;
  text->Draw(std::string(KindLabel(token.kind)) + "  HP " +
                 std::to_string(token.hp) + "/" + std::to_string(token.max_hp) +
                 "  Иниц. " + std::to_string(token.initiative),
             left, cursor_y, kHintText, content_width);

  const SDL_FRect ac_field{left, y + 114.0F, 220.0F, 40.0F};
  const SDL_FRect speed_field{left + 260.0F, y + 114.0F, 220.0F, 40.0F};
  const SDL_FRect attack_field{left, y + 194.0F, 220.0F, 40.0F};
  const SDL_FRect save_field{left + 260.0F, y + 194.0F, 220.0F, 40.0F};
  const SDL_FRect temp_hp_field{left, y + 274.0F, 220.0F, 40.0F};
  const SDL_FRect initiative_field{left + 260.0F, y + 274.0F, 220.0F, 40.0F};
  const SDL_FRect damage_field{left, y + 354.0F, content_width, 40.0F};
  const SDL_FRect notes_field{left, y + 434.0F, content_width, 40.0F};

  DrawModalField(renderer, text, ac_field, std::string(strings::kSheetAc), ac,
                 active == SheetField::kAc);
  DrawModalField(renderer, text, speed_field, std::string(strings::kSheetSpeed),
                 speed, active == SheetField::kSpeed);
  DrawModalField(renderer, text, attack_field,
                 std::string(strings::kSheetAttack), attack,
                 active == SheetField::kAttack);
  DrawModalField(renderer, text, save_field, std::string(strings::kSheetSave),
                 save, active == SheetField::kSave);
  DrawModalField(renderer, text, temp_hp_field,
                 std::string(strings::kSheetTempHp), temp_hp,
                 active == SheetField::kTempHp);
  DrawModalField(renderer, text, initiative_field,
                 std::string(strings::kSheetInitiative), initiative,
                 active == SheetField::kInitiative);
  DrawModalField(renderer, text, damage_field,
                 std::string(strings::kSheetDamage), damage,
                 active == SheetField::kDamage);
  DrawModalField(renderer, text, notes_field, std::string(strings::kSheetNotes),
                 notes, active == SheetField::kNotes);

  text->Draw(std::string(strings::kSheetHint), left, y + height - 30.0F,
             kHintText, content_width);
}

auto DrawBestiaryPanel(SDL_Renderer* renderer, TextRenderer* text,
                       const std::vector<MonsterTemplate>& bestiary) -> void {
  if (renderer == nullptr || text == nullptr) {
    return;
  }
  int window_width = 0;
  int window_height = 0;
  if (!SDL_GetRenderOutputSize(renderer, &window_width, &window_height)) {
    return;
  }
  constexpr float width = 560.0F;
  constexpr float height = 300.0F;
  const float x = (static_cast<float>(window_width) - width) / 2.0F;
  const float y = (static_cast<float>(window_height) - height) / 2.0F;
  const SDL_FRect panel{x, y, width, height};
  SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
  SetDrawColor(renderer, kModalBackground);
  SDL_RenderFillRect(renderer, &panel);
  SetDrawColor(renderer, kPanelBorder);
  SDL_RenderRect(renderer, &panel);
  SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);

  const auto left = x + 30.0F;
  const auto content_width = width - 60.0F;
  auto cursor_y = y + 18.0F;
  text->Draw(std::string(strings::kBestiaryTitle), left, cursor_y, kAuthorText,
             content_width);
  cursor_y += text->LineHeight() + 10.0F;
  const int rows = std::min<int>(5, static_cast<int>(bestiary.size()));
  for (int i = 0; i < rows; ++i) {
    const MonsterTemplate& monster = bestiary[static_cast<std::size_t>(i)];
    const std::string line = std::to_string(i + 1) + ". " + monster.name +
                             "   КД " + std::to_string(monster.ac) + "   HP " +
                             std::to_string(monster.max_hp);
    text->Draw(line, left, cursor_y, kLogText, content_width);
    cursor_y += text->LineHeight() + 2.0F;
  }
  text->Draw(std::string(strings::kBestiaryHint), left, y + height - 30.0F,
             kHintText, content_width);
}

auto DrawConditionsPanel(SDL_Renderer* renderer, TextRenderer* text,
                         const Token& token,
                         const std::vector<Condition>& conditions) -> void {
  if (renderer == nullptr || text == nullptr) {
    return;
  }
  int window_width = 0;
  int window_height = 0;
  if (!SDL_GetRenderOutputSize(renderer, &window_width, &window_height)) {
    return;
  }
  constexpr float width = 520.0F;
  constexpr float height = 360.0F;
  const float x = (static_cast<float>(window_width) - width) / 2.0F;
  const float y = (static_cast<float>(window_height) - height) / 2.0F;
  const SDL_FRect panel{x, y, width, height};
  SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
  SetDrawColor(renderer, kModalBackground);
  SDL_RenderFillRect(renderer, &panel);
  SetDrawColor(renderer, kPanelBorder);
  SDL_RenderRect(renderer, &panel);
  SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);

  const auto left = x + 30.0F;
  const auto content_width = width - 60.0F;
  auto cursor_y = y + 18.0F;
  text->Draw(std::string(strings::kConditions) + ": " + token.name, left,
             cursor_y, kAuthorText, content_width);
  cursor_y += text->LineHeight() + 10.0F;

  const auto& standard = DefaultConditions();
  for (std::size_t i = 0; i < standard.size(); ++i) {
    const auto found = std::ranges::find_if(
        conditions, [&standard, i](const Condition& item) -> bool {
          return item.name == standard[i];
        });
    const bool active = found != conditions.end();
    std::string line =
        std::to_string(i + 1) + ". " + (active ? "[x] " : "[ ] ") + standard[i];
    if (active && found->rounds > 0) {
      line += " (" + std::to_string(found->rounds) + ")";
    }
    text->Draw(line, left, cursor_y, active ? kAuthorText : kLogText,
               content_width);
    cursor_y += text->LineHeight() + 2.0F;
  }
  text->Draw(std::string(strings::kConditionsHint), left, y + height - 30.0F,
             kHintText, content_width);
}

auto DrawScenesPanel(SDL_Renderer* renderer, TextRenderer* text,
                     const std::vector<std::string>& names, std::size_t active,
                     std::size_t selected) -> void {
  if (renderer == nullptr || text == nullptr || names.empty()) {
    return;
  }
  int window_width = 0;
  int window_height = 0;
  if (!SDL_GetRenderOutputSize(renderer, &window_width, &window_height)) {
    return;
  }
  constexpr float width = 620.0F;
  constexpr float height = 380.0F;
  const float x = (static_cast<float>(window_width) - width) / 2.0F;
  const float y = (static_cast<float>(window_height) - height) / 2.0F;
  const SDL_FRect panel{x, y, width, height};
  SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
  SetDrawColor(renderer, kModalBackground);
  SDL_RenderFillRect(renderer, &panel);
  SetDrawColor(renderer, kPanelBorder);
  SDL_RenderRect(renderer, &panel);
  SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);

  const auto left = x + 30.0F;
  const auto content_width = width - 60.0F;
  auto cursor_y = y + 18.0F;
  text->Draw(std::string(strings::kScenesTitle), left, cursor_y, kAuthorText,
             content_width);
  cursor_y += text->LineHeight() + 10.0F;

  // Окно прокрутки держит выбранную сцену в середине списка.
  std::size_t first = 0;
  if (names.size() > kVisibleSceneRows) {
    first = selected > kVisibleSceneRows / 2
                ? selected - (kVisibleSceneRows / 2)
                : 0;
    first = std::min(first, names.size() - kVisibleSceneRows);
  }
  for (std::size_t row = 0; row < kVisibleSceneRows; ++row) {
    const std::size_t index = first + row;
    if (index >= names.size()) {
      break;
    }
    const bool is_active = index == active;
    const bool is_selected = index == selected;
    const std::string line = std::to_string(index + 1) +
                             (is_active ? ". * " : ".   ") +
                             (is_selected ? "> " : "  ") + names[index];
    text->Draw(line, left, cursor_y, is_selected ? kAuthorText : kLogText,
               content_width);
    cursor_y += text->LineHeight() + 2.0F;
  }
  text->DrawWrapped(strings::kScenesHint, left, y + height - 56.0F, kHintText,
                    content_width);
}

auto DrawConnectionBanner(SDL_Renderer* renderer, TextRenderer* text,
                          std::string_view message) -> void {
  if (renderer == nullptr || text == nullptr || message.empty()) {
    return;
  }
  int window_width = 0;
  int window_height = 0;
  if (!SDL_GetRenderOutputSize(renderer, &window_width, &window_height)) {
    return;
  }
  constexpr float width = 520.0F;
  constexpr float padding = 12.0F;
  const float height = text->LineHeight() + (2.0F * padding);
  const float x = (static_cast<float>(window_width) - width) / 2.0F;
  const float y = 72.0F;
  const SDL_FRect panel{x, y, width, height};
  SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
  SetDrawColor(renderer, kPanelBackground);
  SDL_RenderFillRect(renderer, &panel);
  // Красная рамка: это не подсказка, а предупреждение о потере связи.
  SetDrawColor(
      renderer,
      SDL_Color{.r = 0xd0, .g = 0x50, .b = 0x50, .a = SDL_ALPHA_OPAQUE});
  SDL_RenderRect(renderer, &panel);
  SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
  text->DrawCentered(
      message, panel,
      SDL_Color{.r = 0xff, .g = 0xd0, .b = 0xd0, .a = SDL_ALPHA_OPAQUE});
}

auto DrawHelpPanel(SDL_Renderer* renderer, TextRenderer* text, bool is_dm)
    -> void {
  if (renderer == nullptr || text == nullptr) {
    return;
  }
  int window_width = 0;
  int window_height = 0;
  if (!SDL_GetRenderOutputSize(renderer, &window_width, &window_height)) {
    return;
  }
  const std::span<const std::string_view> role_lines =
      is_dm ? std::span<const std::string_view>(strings::kHelpDm)
            : std::span<const std::string_view>(strings::kHelpPlayer);
  const std::size_t rows =
      std::max(strings::kHelpCommon.size(), role_lines.size());

  constexpr float padding = 24.0F;
  constexpr float column_gap = 28.0F;
  const float line_height = text->LineHeight() + 6.0F;
  const float width =
      std::clamp(static_cast<float>(window_width) - 40.0F, 520.0F, 980.0F);
  const float height =
      (2.0F * padding) + (line_height * static_cast<float>(rows + 3));
  const float x = (static_cast<float>(window_width) - width) / 2.0F;
  const float y = (static_cast<float>(window_height) - height) / 2.0F;
  const SDL_FRect panel{x, y, width, height};
  SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
  SetDrawColor(renderer, kModalBackground);
  SDL_RenderFillRect(renderer, &panel);
  SetDrawColor(renderer, kPanelBorder);
  SDL_RenderRect(renderer, &panel);
  SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);

  const float left = x + padding;
  const float column_width = (width - (2.0F * padding) - column_gap) / 2.0F;
  const float right = left + column_width + column_gap;
  float top = y + padding;
  text->Draw(strings::kHelpTitle, left, top, kInputText,
             width - (2.0F * padding));
  top += line_height;
  text->Draw(strings::kHelpCommonTitle, left, top, kAuthorText, column_width);
  text->Draw(is_dm ? strings::kHelpDmTitle : strings::kHelpPlayerTitle, right,
             top, kAuthorText, column_width);
  top += line_height;
  for (std::size_t i = 0; i < rows; ++i) {
    const float row_y = top + (static_cast<float>(i) * line_height);
    if (i < strings::kHelpCommon.size()) {
      text->Draw(std::string("• ") + std::string(strings::kHelpCommon[i]), left,
                 row_y, kLogText, column_width);
    }
    if (i < role_lines.size()) {
      text->Draw(std::string("• ") + std::string(role_lines[i]), right, row_y,
                 kLogText, column_width);
    }
  }
  const float hint_y = y + height - padding - line_height;
  text->Draw(strings::kHelpClose, left, hint_y, kHintText,
             width - (2.0F * padding));
}

}  // namespace dnd
