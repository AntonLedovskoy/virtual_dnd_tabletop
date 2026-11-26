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
#include "core/levels.h"
#include "core/player_name.h"
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
constexpr SDL_Color kErrorText{
    .r = 0xff, .g = 0x90, .b = 0x90, .a = SDL_ALPHA_OPAQUE};

constexpr int kPadding = 12;
constexpr std::size_t kVisibleSceneRows = 6;
constexpr int kLineSpacing = 4;
// Геометрия панелей общая для отрисовки и попадания мышью.
constexpr int kPanelPadding = 10;
constexpr int kInitiativePanelWidth = 230;
constexpr int kInitiativePanelTop = 12;
constexpr int kInitiativeMaxRows = 10;
constexpr int kPlayersPanelWidth = 220;
constexpr int kPlayersPanelX = 12;
constexpr int kPlayersMaxRows = 8;
constexpr float kScenesPanelWidth = 760.0F;
constexpr float kScenesPanelHeight = 420.0F;
constexpr float kCombatPanelWidth = 640.0F;
constexpr float kCombatPanelHeight = 460.0F;
constexpr std::size_t kVisibleCombatRows = 14;

auto SetDrawColor(SDL_Renderer* renderer, SDL_Color color) -> void {
  SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
}

auto ParticipantsInTurnOrder(const std::vector<Token>& tokens,
                             const Encounter& encounter)
    -> std::vector<const Token*> {
  // В панели только участники боя и в том порядке, в каком они ходят. Токен,
  // скрытый туманом игрока, в его срезе отсутствует — строку не рисуем.
  std::vector<const Token*> ordered;
  ordered.reserve(encounter.order.size());
  for (const TokenId id : encounter.order) {
    for (const Token& token : tokens) {
      if (token.id == id) {
        ordered.push_back(&token);
        break;
      }
    }
  }
  return ordered;
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

constexpr float kHistoryPadding = 24.0F;

// Раскладка модального журнала: панель ужимается под высоту окна, поэтому
// на маленьком логическом экране видно меньше строк, а подсказка не обрезана.
struct HistoryLayout {
  float width = 0.0F;
  float height = 0.0F;
  float line_height = 0.0F;
  float hint_height = 0.0F;
  int rows = 0;
};

auto BuildHistoryLayout(TextRenderer* text, int window_width, int window_height)
    -> HistoryLayout {
  constexpr int line_spacing = 3;
  constexpr float window_margin = 20.0F;
  const float line_height = text->LineHeight() + line_spacing;
  const float width =
      std::clamp(static_cast<float>(window_width) - 80.0F, 560.0F, 1100.0F);
  const float content_width = width - (2.0F * kHistoryPadding);
  // Подсказка измеряется: при узком окне она переносится на две строки, и
  // фиксированный запас в две строки её обрезал.
  const float hint_height =
      std::max(line_height,
               text->Measure(strings::kLogHistoryHint, content_width).height);
  const float fixed_height =
      (2.0F * kHistoryPadding) + (3.0F * line_height) + hint_height;
  const float available =
      std::max(fixed_height + line_height,
               static_cast<float>(window_height) - (2.0F * window_margin));
  const int rows = std::clamp(static_cast<int>((available - fixed_height) /
                                               std::max(1.0F, line_height)),
                              1, kLogHistoryVisibleRows);
  return HistoryLayout{
      .width = width,
      .height = fixed_height + (static_cast<float>(rows) * line_height),
      .line_height = line_height,
      .hint_height = hint_height,
      .rows = rows,
  };
}

}  // namespace

auto RenderSize(SDL_Renderer* renderer, int& width, int& height) -> bool {
  if (renderer == nullptr) {
    return false;
  }
  int logical_width = 0;
  int logical_height = 0;
  SDL_RendererLogicalPresentation mode = SDL_LOGICAL_PRESENTATION_DISABLED;
  if (SDL_GetRenderLogicalPresentation(renderer, &logical_width,
                                       &logical_height, &mode) &&
      mode != SDL_LOGICAL_PRESENTATION_DISABLED && logical_width > 0 &&
      logical_height > 0) {
    width = logical_width;
    height = logical_height;
    return true;
  }
  return SDL_GetRenderOutputSize(renderer, &width, &height);
}

auto DrawLogPanel(SDL_Renderer* renderer, TextRenderer* text,
                  const std::vector<LogLine>& lines, bool typing,
                  const std::string& input, std::string_view hint, int scroll)
    -> float {
  int window_width = 0;
  int window_height = 0;
  if (!RenderSize(renderer, window_width, window_height)) {
    return 0.0F;
  }
  if (window_width <= 0 || window_height <= 0) {
    return 0.0F;
  }

  const auto max_text_width =
      static_cast<float>(window_width - (2 * kPadding)) - 160.0F;
  const int line_height =
      text != nullptr ? static_cast<int>(text->LineHeight()) : 16;
  // При вводе на месте подсказки стоит строка чата: место под неё всё равно
  // нужно, поэтому по умолчанию считаем одну строку.
  int hint_line_count = 1;
  if (!typing && text != nullptr) {
    hint_line_count = 0;
    if (!hint.empty()) {
      const float hint_height = text->Measure(hint, max_text_width).height;
      const float row_height =
          std::max(1.0F, text->LineHeight() + kLineSpacing);
      hint_line_count =
          std::max(1, static_cast<int>(std::ceil(hint_height / row_height)));
    }
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
    } else if (!hint.empty()) {
      text->DrawWrapped(hint, static_cast<float>(kPadding), input_y, kHintText,
                        max_text_width);
    }
  }
  return panel_top;
}

auto DrawLogHistoryPanel(SDL_Renderer* renderer, TextRenderer* text,
                         const std::vector<HistoryLine>& lines,
                         const std::vector<std::size_t>& matches,
                         std::string_view query, int scroll) -> void {
  if (renderer == nullptr || text == nullptr) {
    return;
  }
  int window_width = 0;
  int window_height = 0;
  if (!RenderSize(renderer, window_width, window_height)) {
    return;
  }
  const HistoryLayout layout =
      BuildHistoryLayout(text, window_width, window_height);
  const float line_height = layout.line_height;
  const float width = layout.width;
  const float height = layout.height;
  const float x = (static_cast<float>(window_width) - width) / 2.0F;
  const float y = (static_cast<float>(window_height) - height) / 2.0F;
  const SDL_FRect panel{x, y, width, height};
  SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
  SetDrawColor(renderer, kModalBackground);
  SDL_RenderFillRect(renderer, &panel);
  SetDrawColor(renderer, kPanelBorder);
  SDL_RenderRect(renderer, &panel);
  SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);

  const float left = x + kHistoryPadding;
  const float content_width = width - (2.0F * kHistoryPadding);
  auto cursor_y = y + kHistoryPadding;
  text->Draw(strings::kLogHistoryTitle, left, cursor_y, kInputText,
             content_width);
  cursor_y += line_height;
  text->Draw(std::string(strings::kLogHistorySearch) + std::string(query) + "_",
             left, cursor_y, kInputText, content_width);
  cursor_y += line_height;

  std::string count;
  if (lines.empty()) {
    count = std::string(strings::kLogHistoryEmpty);
  } else if (query.empty()) {
    count =
        std::string(strings::kLogHistoryCount) + std::to_string(lines.size());
  } else {
    count = std::string(strings::kLogHistoryFound) +
            std::to_string(matches.size()) +
            std::string(strings::kLogHistoryOf) + std::to_string(lines.size());
  }
  text->Draw(count, left, cursor_y, kHintText, content_width);
  cursor_y += line_height;

  const int total = static_cast<int>(matches.size());
  const int max_scroll = std::max(0, total - layout.rows);
  const int offset = std::clamp(scroll, 0, max_scroll);
  const int first = std::max(0, total - layout.rows - offset);
  const int last = std::min(total, first + layout.rows);
  for (int i = first; i < last; ++i) {
    const HistoryLine& line = lines[matches[static_cast<std::size_t>(i)]];
    if (line.header) {
      const std::string header =
          std::string(strings::kLogHistorySessionPrefix) + line.time +
          std::string(strings::kLogHistorySessionSuffix);
      text->Draw(header, left, cursor_y, kAuthorText, content_width);
    } else {
      auto row_x = left;
      if (!line.time.empty()) {
        const std::string stamp = "[" + line.time + "] ";
        text->Draw(stamp, left, cursor_y, kHintText, content_width);
        row_x += text->Measure(stamp).width;
      }
      if (!line.author.empty()) {
        const std::string author = line.author + ":";
        const float available = content_width - (row_x - left);
        if (available > 0.0F) {
          text->Draw(author, row_x, cursor_y, kAuthorText, available);
        }
        row_x += text->Measure(author + " ").width;
      }
      const float available = content_width - (row_x - left);
      if (available > 0.0F) {
        text->Draw(line.text, row_x, cursor_y, kLogText, available);
      }
    }
    cursor_y += line_height;
  }

  const float hint_y = y + height - kHistoryPadding - layout.hint_height;
  text->DrawWrapped(strings::kLogHistoryHint, left, hint_y, kHintText,
                    content_width);
}

auto LogHistoryVisibleRows(SDL_Renderer* renderer, TextRenderer* text) -> int {
  if (renderer == nullptr || text == nullptr) {
    return kLogHistoryVisibleRows;
  }
  int window_width = 0;
  int window_height = 0;
  if (!RenderSize(renderer, window_width, window_height) || window_width <= 0 ||
      window_height <= 0) {
    return kLogHistoryVisibleRows;
  }
  return BuildHistoryLayout(text, window_width, window_height).rows;
}

auto DrawInitiativePanel(SDL_Renderer* renderer, TextRenderer* text,
                         const std::vector<Token>& tokens,
                         const Encounter& encounter) -> void {
  if (text == nullptr || tokens.empty()) {
    return;
  }
  int window_width = 0;
  int window_height = 0;
  if (!RenderSize(renderer, window_width, window_height)) {
    return;
  }
  if (window_width <= 0 || window_height <= 0) {
    return;
  }

  std::vector<const Token*> sorted = ParticipantsInTurnOrder(tokens, encounter);

  const int line_height = static_cast<int>(text->LineHeight()) + 4;
  const int rows =
      std::min<int>(kInitiativeMaxRows, static_cast<int>(sorted.size()));
  const int panel_height = ((rows + 1) * line_height) + (2 * kPanelPadding);
  const auto panel_x =
      static_cast<float>(window_width - kInitiativePanelWidth - 12);
  const auto panel_top = static_cast<float>(kInitiativePanelTop);
  const SDL_FRect panel{
      .x = panel_x,
      .y = panel_top,
      .w = static_cast<float>(kInitiativePanelWidth),
      .h = static_cast<float>(panel_height),
  };
  SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
  SetDrawColor(renderer, kPanelBackground);
  SDL_RenderFillRect(renderer, &panel);
  SetDrawColor(renderer, kPanelBorder);
  SDL_RenderRect(renderer, &panel);
  SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);

  const auto text_x = panel_x + static_cast<float>(kPanelPadding);
  const auto max_text_width =
      static_cast<float>(kInitiativePanelWidth - (2 * kPanelPadding));
  auto y = panel_top + static_cast<float>(kPanelPadding);
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
                    bool is_client, bool can_claim, MovementInfo movement,
                    bool in_combat) -> float {
  if (renderer == nullptr || text == nullptr || token == nullptr) {
    return 0.0F;
  }
  int window_width = 0;
  int window_height = 0;
  if (!RenderSize(renderer, window_width, window_height)) {
    return 0.0F;
  }
  if (window_width <= 0 || window_height <= 0) {
    return 0.0F;
  }

  const std::string owner =
      token->owner.empty() ? std::string(strings::kFree) : token->owner;
  const int level = LevelForXp(token->xp);
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
             std::to_string(token->initiative) + "   " +
             std::string(strings::kLevelShort) + " " + std::to_string(level);
  lines.push_back(std::move(hp_line));
  if (token->kind == TokenKind::kPlayer) {
    std::string xp_line =
        std::string(strings::kXpShort) + ": " + std::to_string(token->xp) + "/";
    xp_line += level < kMaxLevel ? std::to_string(XpForLevel(level + 1))
                                 : std::string(strings::kXpMax);
    lines.push_back(std::move(xp_line));
    lines.emplace_back(
        std::string(strings::kHitDiceShort) + ": " +
        std::to_string(HitDiceAvailable(level, token->hit_dice_spent)) + "/" +
        std::to_string(HitDiceMaximum(level)));
  }
  lines.emplace_back(std::string(strings::kSheetAc) + ": " +
                     std::to_string(token->ac) + "   " +
                     std::string(strings::kSheetSpeed) + ": " +
                     std::to_string(token->speed));
  if (movement == MovementInfo::kBudget) {
    // Запас с учётом истощения и обездвиживающих состояний.
    const int budget = EffectiveSpeedCells(*token) + token->movement_bonus;
    const int left = std::max(0, budget - token->movement_used);
    lines.emplace_back(
        std::string(strings::kMovementLeft) + ": " + FormatMeters(left) + " " +
        std::string(strings::kMovementOf) + " " + FormatMeters(budget));
  } else if (movement == MovementInfo::kFree) {
    lines.emplace_back(strings::kMovementFree);
  }
  if (in_combat) {
    const auto counter = [](bool used) -> std::string_view {
      return used ? strings::kCounterSpent : strings::kCounterAvailable;
    };
    lines.emplace_back(std::string(strings::kActionCounter) + ": " +
                       std::string(counter(token->action_used)) + "   " +
                       std::string(strings::kActionBonusShort) + ": " +
                       std::string(counter(token->bonus_action_used)));
    lines.emplace_back(std::string(strings::kActionReaction) + ": " +
                       std::string(counter(token->reaction_used)));
  }
  if (!token->conditions.empty()) {
    std::string condition_line = std::string(strings::kConditions) + ": ";
    for (std::size_t i = 0; i < token->conditions.size(); ++i) {
      if (i > 0) {
        condition_line += ", ";
      }
      condition_line += token->conditions[i].name;
      if (token->conditions[i].seconds > 0) {
        condition_line +=
            " (" + FormatConditionDuration(token->conditions[i].seconds) + ")";
      }
    }
    lines.push_back(std::move(condition_line));
  }

  std::vector<std::string> hints;
  if (is_client && can_claim) {
    hints.emplace_back(strings::kClaimHint);
  }
  if (is_client && SamePlayerName(token->owner, player_name)) {
    hints.emplace_back(strings::kYourToken);
  }
  if (!is_client) {
    hints.emplace_back(strings::kDmSpawnHint);
    hints.emplace_back(strings::kDmCommandHint);
    hints.emplace_back(strings::kColorHint);
  }

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
  const auto panel_padding = static_cast<float>(kPanelPadding);
  const int line_height = static_cast<int>(text->LineHeight()) + 4;
  const int rows =
      std::min<int>(kPlayersMaxRows, static_cast<int>(players.size()));
  const int panel_height = ((rows + 1) * line_height) + (2 * kPanelPadding);

  SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
  const SDL_FRect panel{
      .x = static_cast<float>(kPlayersPanelX),
      .y = panel_y,
      .w = static_cast<float>(kPlayersPanelWidth),
      .h = static_cast<float>(panel_height),
  };
  SetDrawColor(renderer, kPanelBackground);
  SDL_RenderFillRect(renderer, &panel);
  SetDrawColor(renderer, kPanelBorder);
  SDL_RenderRect(renderer, &panel);
  SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);

  const auto text_x = static_cast<float>(kPlayersPanelX) + panel_padding;
  const auto max_text_width =
      static_cast<float>(kPlayersPanelWidth - (2 * kPanelPadding));
  auto y = panel_y + panel_padding;
  text->Draw(std::string(strings::kPlayers), text_x, y, kAuthorText,
             max_text_width);
  y += static_cast<float>(line_height);
  for (int i = 0; i < rows; ++i) {
    text->Draw(players[static_cast<std::size_t>(i)], text_x, y, kLogText,
               max_text_width);
    y += static_cast<float>(line_height);
  }
}

auto InitiativeTokenAt(SDL_Renderer* renderer, TextRenderer* text,
                       const std::vector<Token>& tokens,
                       const Encounter& encounter, float px, float py)
    -> TokenId {
  if (renderer == nullptr || text == nullptr || tokens.empty()) {
    return 0;
  }
  int window_width = 0;
  int window_height = 0;
  if (!RenderSize(renderer, window_width, window_height)) {
    return 0;
  }
  const auto panel_x =
      static_cast<float>(window_width - kInitiativePanelWidth - 12);
  if (px < panel_x || px > panel_x + kInitiativePanelWidth) {
    return 0;
  }
  std::vector<const Token*> sorted = ParticipantsInTurnOrder(tokens, encounter);
  if (sorted.empty()) {
    return 0;
  }
  const int line_height = static_cast<int>(text->LineHeight()) + 4;
  const int rows =
      std::min<int>(kInitiativeMaxRows, static_cast<int>(sorted.size()));
  const auto first_row_y =
      static_cast<float>(kInitiativePanelTop + kPanelPadding + line_height);
  if (py < first_row_y) {
    return 0;
  }
  const auto row =
      static_cast<int>((py - first_row_y) / static_cast<float>(line_height));
  if (row < 0 || row >= rows) {
    return 0;
  }
  return sorted[static_cast<std::size_t>(row)]->id;
}

auto PlayerRowAt(SDL_Renderer* renderer, TextRenderer* text,
                 std::size_t players, float panel_y, float px, float py)
    -> int {
  if (renderer == nullptr || text == nullptr || players == 0) {
    return -1;
  }
  if (px < kPlayersPanelX || px > kPlayersPanelX + kPlayersPanelWidth) {
    return -1;
  }
  const int line_height = static_cast<int>(text->LineHeight()) + 4;
  const int rows = std::min<int>(kPlayersMaxRows, static_cast<int>(players));
  const auto first_row_y =
      panel_y + static_cast<float>(kPanelPadding + line_height);
  if (py < first_row_y) {
    return -1;
  }
  const auto row =
      static_cast<int>((py - first_row_y) / static_cast<float>(line_height));
  if (row < 0 || row >= rows) {
    return -1;
  }
  return row;
}

auto SceneRowAt(SDL_Renderer* renderer, TextRenderer* text,
                const std::vector<Scene>& scenes, std::size_t selected,
                float px, float py) -> std::size_t {
  if (renderer == nullptr || text == nullptr || scenes.empty()) {
    return kNoScene;
  }
  int window_width = 0;
  int window_height = 0;
  if (!RenderSize(renderer, window_width, window_height)) {
    return kNoScene;
  }
  const float x = (static_cast<float>(window_width) - kScenesPanelWidth) / 2.0F;
  const float y =
      (static_cast<float>(window_height) - kScenesPanelHeight) / 2.0F;
  if (px < x || px > x + kScenesPanelWidth || py < y ||
      py > y + kScenesPanelHeight) {
    return kNoScene;
  }
  std::size_t first = 0;
  if (scenes.size() > kVisibleSceneRows) {
    first = selected > kVisibleSceneRows / 2
                ? selected - (kVisibleSceneRows / 2)
                : 0;
    first = std::min(first, scenes.size() - kVisibleSceneRows);
  }
  const float rows_top = y + 18.0F + text->LineHeight() + 10.0F;
  const float row_height =
      (text->LineHeight() + 2.0F) + (text->LineHeight() + 6.0F);
  const float rows_bottom = y + kScenesPanelHeight - 56.0F;
  if (py < rows_top || py >= rows_bottom) {
    return kNoScene;
  }
  const auto row = static_cast<std::size_t>((py - rows_top) / row_height);
  if (row >= kVisibleSceneRows) {
    return kNoScene;
  }
  const std::size_t index = first + row;
  return index < scenes.size() ? index : kNoScene;
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
                    const std::string& swim, const std::string& climb,
                    const std::string& attack, const std::string& save,
                    const std::string& initiative, const std::string& temp_hp,
                    const std::string& hit_die, const std::string& con,
                    const std::string& range, const std::string& damage,
                    const std::string& resist, const std::string& vulnerable,
                    const std::string& immune, const std::string& notes,
                    const std::string& darkvision, bool read_only) -> void {
  if (renderer == nullptr || text == nullptr) {
    return;
  }
  int window_width = 0;
  int window_height = 0;
  if (!RenderSize(renderer, window_width, window_height)) {
    return;
  }
  constexpr float width = 640.0F;
  constexpr float height = 600.0F;
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
  cursor_y += text->LineHeight() + 6.0F;
  text->Draw(std::string(KindLabel(token.kind)) + "  HP " +
                 std::to_string(token.hp) + "/" + std::to_string(token.max_hp) +
                 "  Иниц. " + std::to_string(token.initiative) + "  " +
                 std::string(strings::kSheetLevel) + " " +
                 std::to_string(LevelForXp(token.xp)) + "  " +
                 std::string(strings::kSheetXp) + " " +
                 std::to_string(token.xp),
             left, cursor_y, kHintText, content_width);
  cursor_y += text->LineHeight() + 2.0F;
  if (token.kind == TokenKind::kPlayer) {
    // Бонус мастерства уже включён в итоговые бонусы листа, здесь он только
    // справочный. Монстры опыт не получают, им блок не нужен.
    text->Draw(std::string(strings::kSheetProficiency) + " +" +
                   std::to_string(ProficiencyBonus(LevelForXp(token.xp))) +
                   "   " + std::string(strings::kSheetPerLevel) + " +" +
                   std::to_string(
                       HitPointsPerLevel(token.hit_die, token.con_modifier)),
               left, cursor_y, kHintText, content_width);
  }

  const SDL_FRect ac_field{left, y + 100.0F, 174.0F, 38.0F};
  const SDL_FRect speed_field{left + 193.0F, y + 100.0F, 174.0F, 38.0F};
  const SDL_FRect initiative_field{left + 386.0F, y + 100.0F, 174.0F, 38.0F};
  const SDL_FRect swim_field{left, y + 148.0F, 174.0F, 38.0F};
  const SDL_FRect climb_field{left + 193.0F, y + 148.0F, 174.0F, 38.0F};
  const SDL_FRect attack_field{left + 386.0F, y + 148.0F, 174.0F, 38.0F};
  const SDL_FRect save_field{left, y + 196.0F, 174.0F, 38.0F};
  const SDL_FRect temp_hp_field{left + 193.0F, y + 196.0F, 174.0F, 38.0F};
  const SDL_FRect hit_die_field{left + 386.0F, y + 196.0F, 174.0F, 38.0F};
  const SDL_FRect con_field{left, y + 244.0F, 174.0F, 38.0F};
  const SDL_FRect range_field{left + 193.0F, y + 244.0F, 174.0F, 38.0F};
  const SDL_FRect darkvision_field{left + 386.0F, y + 244.0F, 174.0F, 38.0F};
  const SDL_FRect damage_field{left, y + 296.0F, content_width, 38.0F};
  const SDL_FRect resist_field{left, y + 344.0F, content_width, 38.0F};
  const SDL_FRect vulnerable_field{left, y + 392.0F, content_width, 38.0F};
  const SDL_FRect immune_field{left, y + 440.0F, content_width, 38.0F};
  const SDL_FRect notes_field{left, y + 488.0F, content_width, 38.0F};

  const bool editing = !read_only;
  DrawModalField(renderer, text, ac_field, std::string(strings::kSheetAc), ac,
                 editing && active == SheetField::kAc);
  DrawModalField(renderer, text, speed_field, std::string(strings::kSheetSpeed),
                 speed, editing && active == SheetField::kSpeed);
  DrawModalField(renderer, text, initiative_field,
                 std::string(strings::kSheetInitiative), initiative,
                 editing && active == SheetField::kInitiative);
  DrawModalField(renderer, text, swim_field, std::string(strings::kSheetSwim),
                 swim, editing && active == SheetField::kSwim);
  DrawModalField(renderer, text, climb_field, std::string(strings::kSheetClimb),
                 climb, editing && active == SheetField::kClimb);
  DrawModalField(renderer, text, attack_field,
                 std::string(strings::kSheetAttack), attack,
                 editing && active == SheetField::kAttack);
  DrawModalField(renderer, text, save_field, std::string(strings::kSheetSave),
                 save, editing && active == SheetField::kSave);
  DrawModalField(renderer, text, temp_hp_field,
                 std::string(strings::kSheetTempHp), temp_hp,
                 editing && active == SheetField::kTempHp);
  DrawModalField(renderer, text, hit_die_field,
                 std::string(strings::kSheetHitDie), hit_die,
                 editing && active == SheetField::kHitDie);
  DrawModalField(renderer, text, con_field, std::string(strings::kSheetCon),
                 con, editing && active == SheetField::kCon);
  DrawModalField(renderer, text, range_field, std::string(strings::kSheetRange),
                 range, editing && active == SheetField::kRange);
  DrawModalField(renderer, text, darkvision_field,
                 std::string(strings::kSheetDarkvision), darkvision,
                 editing && active == SheetField::kDarkvision);
  DrawModalField(renderer, text, damage_field,
                 std::string(strings::kSheetDamage), damage,
                 editing && active == SheetField::kDamage);
  DrawModalField(renderer, text, resist_field,
                 std::string(strings::kSheetResist), resist,
                 editing && active == SheetField::kResist);
  DrawModalField(renderer, text, vulnerable_field,
                 std::string(strings::kSheetVulnerable), vulnerable,
                 editing && active == SheetField::kVulnerable);
  DrawModalField(renderer, text, immune_field,
                 std::string(strings::kSheetImmune), immune,
                 editing && active == SheetField::kImmune);
  DrawModalField(renderer, text, notes_field, std::string(strings::kSheetNotes),
                 notes, editing && active == SheetField::kNotes);

  text->Draw(
      std::string(read_only ? strings::kSheetViewHint : strings::kSheetHint),
      left, y + height - 30.0F, kHintText, content_width);
}

auto DrawBestiaryPanel(SDL_Renderer* renderer, TextRenderer* text,
                       const std::vector<MonsterTemplate>& bestiary) -> void {
  if (renderer == nullptr || text == nullptr) {
    return;
  }
  int window_width = 0;
  int window_height = 0;
  if (!RenderSize(renderer, window_width, window_height)) {
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
  const int rows = std::min<int>(9, static_cast<int>(bestiary.size()));
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
                         const std::vector<Condition>& conditions,
                         bool read_only, std::size_t selected, bool edit_dc,
                         const std::string& dc_buffer, bool edit_custom,
                         const std::string& custom_buffer) -> void {
  if (renderer == nullptr || text == nullptr) {
    return;
  }
  int window_width = 0;
  int window_height = 0;
  if (!RenderSize(renderer, window_width, window_height)) {
    return;
  }
  constexpr float width = 660.0F;
  constexpr float height = 480.0F;
  const float x = (static_cast<float>(window_width) - width) / 2.0F;
  const float y = (static_cast<float>(window_height) - height) / 2.0F;
  const SDL_FRect panel{x, y, width, height};
  SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
  SetDrawColor(renderer, kModalBackground);
  SDL_RenderFillRect(renderer, &panel);
  SetDrawColor(renderer, kPanelBorder);
  SDL_RenderRect(renderer, &panel);
  SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);

  const auto left = x + 26.0F;
  const auto content_width = width - 52.0F;
  auto cursor_y = y + 16.0F;
  text->Draw(std::string(strings::kConditions) + ": " + token.name, left,
             cursor_y, kAuthorText, content_width);
  cursor_y += text->LineHeight() + 4.0F;
  const int exhaustion = ExhaustionLevel(token);
  std::string summary = "Истощение: " + std::to_string(exhaustion) + "/" +
                        std::to_string(kMaxExhaustionLevel);
  if (exhaustion > 0) {
    summary += " (−" + std::to_string(ExhaustionPenalty(token)) + " к d20, −" +
               std::to_string(ExhaustionSpeedPenaltyCells(token)) +
               " кл. скорости)";
  }
  if (IsSpeedZeroed(token)) {
    summary += "  Скорость 0";
  }
  text->Draw(summary, left, cursor_y, kHintText, content_width);

  const auto& standard = DefaultConditions();
  // Окно списка: восемь строк вокруг выбранной.
  constexpr std::size_t visible_rows = 8;
  std::size_t first = 0;
  if (selected >= visible_rows) {
    first = selected - visible_rows + 1;
  }
  if (standard.size() > visible_rows &&
      first + visible_rows > standard.size()) {
    first = standard.size() - visible_rows;
  }
  const float list_top = y + 70.0F;
  const float left_width = 290.0F;
  auto row_y = list_top;
  for (std::size_t i = first; i < standard.size() && i < first + visible_rows;
       ++i) {
    const auto found = std::ranges::find_if(
        conditions, [&standard, i](const Condition& item) -> bool {
          return item.name == standard[i];
        });
    const bool active = found != conditions.end();
    std::string line;
    if (!read_only) {
      line = (i == selected ? "> " : "  ") + std::to_string(i + 1) + ". ";
    }
    line += (active ? "[x] " : "[ ] ") + standard[i];
    if (standard[i] == kExhaustion) {
      line += " (" + std::to_string(exhaustion) + "/" +
              std::to_string(kMaxExhaustionLevel) + ")";
    } else if (active) {
      line += " (" + FormatConditionDuration(found->seconds);
      if (found->save_dc > 0) {
        line += ", КС " + std::to_string(found->save_dc);
      }
      line += ")";
    }
    text->Draw(line, left, row_y, active ? kAuthorText : kLogText, left_width);
    row_y += text->LineHeight() + 2.0F;
  }
  // Правая колонка: всё, что сейчас на токене, включая свои состояния.
  const float right_x = left + left_width + 16.0F;
  const float right_width = width - (right_x - x) - 20.0F;
  text->Draw("Сейчас на токене:", right_x, list_top, kHintText, right_width);
  auto active_y = list_top + text->LineHeight() + 2.0F;
  std::size_t shown = 0;
  bool exhaustion_shown = false;
  for (const Condition& condition : conditions) {
    if (condition.name == kExhaustion) {
      if (exhaustion_shown) {
        continue;
      }
      exhaustion_shown = true;
    }
    if (shown >= visible_rows) {
      text->Draw("…", right_x, active_y, kHintText, right_width);
      break;
    }
    std::string line = "• " + condition.name;
    if (condition.name != kExhaustion) {
      line += " — " + FormatConditionDuration(condition.seconds);
      if (condition.save_dc > 0) {
        line += ", КС " + std::to_string(condition.save_dc);
      }
    } else {
      line += " — уровень " + std::to_string(ExhaustionLevel(token));
    }
    text->Draw(line, right_x, active_y, kLogText, right_width);
    active_y += text->LineHeight() + 2.0F;
    ++shown;
  }
  if (conditions.empty()) {
    text->Draw("нет состояний", right_x, active_y, kHintText, right_width);
  }

  if (read_only) {
    text->Draw(std::string(strings::kConditionsViewHint), left,
               y + height - 30.0F, kHintText, content_width);
    return;
  }
  if (edit_dc) {
    text->Draw(std::string(strings::kConditionDcPrompt) + dc_buffer + "_", left,
               y + height - 98.0F, kAuthorText, content_width);
  } else if (edit_custom) {
    text->Draw(
        std::string(strings::kConditionCustomPrompt) + custom_buffer + "_",
        left, y + height - 98.0F, kAuthorText, content_width);
  }
  text->Draw(std::string(strings::kConditionsPanelHint), left,
             y + height - 70.0F, kHintText, content_width);
  text->Draw(std::string(strings::kConditionsPanelHint2), left,
             y + height - 46.0F, kHintText, content_width);
  text->Draw(std::string(strings::kConditionsPanelHint3), left,
             y + height - 22.0F, kHintText, content_width);
}

auto DrawActionsPanel(SDL_Renderer* renderer, TextRenderer* text,
                      const Token& token, bool own_turn) -> void {
  if (renderer == nullptr || text == nullptr) {
    return;
  }
  int window_width = 0;
  int window_height = 0;
  if (!RenderSize(renderer, window_width, window_height)) {
    return;
  }
  constexpr float width = 560.0F;
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
  text->Draw(std::string(strings::kActionsTitle) + ": " + token.name, left,
             cursor_y, kAuthorText, content_width);
  cursor_y += text->LineHeight() + 8.0F;

  std::string_view status = strings::kActionAvailable;
  if (token.action_used) {
    status = strings::kActionSpent;
  } else if (!own_turn) {
    status = strings::kActionNotTurn;
  }
  text->Draw(std::string(status), left, cursor_y,
             token.action_used || !own_turn ? kHintText : kLogText,
             content_width);
  cursor_y += text->LineHeight() + 4.0F;

  const auto counter = [](bool used) -> std::string_view {
    return used ? strings::kCounterSpent : strings::kCounterAvailable;
  };
  const std::string counters = std::string(strings::kActionBonus) + ": " +
                               std::string(counter(token.bonus_action_used)) +
                               "   " + std::string(strings::kActionReaction) +
                               ": " + std::string(counter(token.reaction_used));
  text->Draw(counters, left, cursor_y, kLogText, content_width);
  cursor_y += text->LineHeight() + 4.0F;

  if (token.movement_bonus > 0) {
    text->Draw(std::string(strings::kActionDashBonus) +
                   FormatMeters(token.movement_bonus),
               left, cursor_y, kLogText, content_width);
    cursor_y += text->LineHeight() + 4.0F;
  }
  cursor_y += 4.0F;

  for (std::size_t i = 0; i < strings::kActionNames.size(); ++i) {
    const std::string line = std::to_string(i + 1) + ". " +
                             std::string(strings::kActionNames[i]) + " — " +
                             std::string(strings::kActionNotes[i]);
    text->Draw(line, left, cursor_y, kLogText, content_width);
    cursor_y += text->LineHeight() + 2.0F;
  }
  text->Draw(std::string(strings::kActionsHint), left, y + height - 30.0F,
             kHintText, content_width);
}

auto DrawCombatPanel(SDL_Renderer* renderer, TextRenderer* text,
                     const std::vector<Token>& tokens,
                     const Encounter& encounter, std::size_t selected) -> void {
  if (renderer == nullptr || text == nullptr || tokens.empty()) {
    return;
  }
  int window_width = 0;
  int window_height = 0;
  if (!RenderSize(renderer, window_width, window_height)) {
    return;
  }
  constexpr float width = kCombatPanelWidth;
  constexpr float height = kCombatPanelHeight;
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
  text->Draw(std::string(strings::kCombatTitle), left, cursor_y, kAuthorText,
             content_width);
  cursor_y += text->LineHeight() + 6.0F;
  const std::string count = std::string(strings::kCombatRoundPrefix) +
                            std::to_string(encounter.round) +
                            std::string(strings::kCombatJoined) +
                            std::to_string(encounter.order.size()) + "/" +
                            std::to_string(tokens.size());
  text->Draw(count, left, cursor_y, kHintText, content_width);
  cursor_y += text->LineHeight() + 10.0F;

  // Окно прокрутки держит выбранный токен в середине списка.
  std::size_t first = 0;
  if (tokens.size() > kVisibleCombatRows) {
    first = selected > kVisibleCombatRows / 2
                ? selected - (kVisibleCombatRows / 2)
                : 0;
    first = std::min(first, tokens.size() - kVisibleCombatRows);
  }
  const float line_step = text->LineHeight() + 4.0F;
  for (std::size_t row = 0; row < kVisibleCombatRows; ++row) {
    const std::size_t index = first + row;
    if (index >= tokens.size()) {
      break;
    }
    const Token& token = tokens[index];
    const bool joined =
        std::ranges::find(encounter.order, token.id) != encounter.order.end();
    const bool is_selected = index == selected;
    const std::string line =
        (joined ? "[+] " : "[ ] ") + token.name + "   " +
        std::string(strings::kInitiativeShort) + " " +
        (joined ? std::to_string(token.initiative) : std::string("—")) + "   " +
        std::string(strings::kHp) + " " + std::to_string(token.hp) + "/" +
        std::to_string(token.max_hp);
    text->Draw(line, left, cursor_y, is_selected ? kAuthorText : kLogText,
               content_width);
    cursor_y += line_step;
  }
  text->DrawWrapped(std::string(strings::kCombatHint), left, y + height - 34.0F,
                    kHintText, content_width);
}

auto DrawScenesPanel(SDL_Renderer* renderer, TextRenderer* text,
                     const std::vector<Scene>& scenes, std::size_t active,
                     std::size_t selected, const std::string& edit_label,
                     const std::string& edit_text, const std::string& status,
                     std::string_view hint) -> void {
  if (renderer == nullptr || text == nullptr || scenes.empty()) {
    return;
  }
  int window_width = 0;
  int window_height = 0;
  if (!RenderSize(renderer, window_width, window_height)) {
    return;
  }
  constexpr float width = kScenesPanelWidth;
  constexpr float height = kScenesPanelHeight;
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
  if (scenes.size() > kVisibleSceneRows) {
    first = selected > kVisibleSceneRows / 2
                ? selected - (kVisibleSceneRows / 2)
                : 0;
    first = std::min(first, scenes.size() - kVisibleSceneRows);
  }
  const float name_step = text->LineHeight() + 2.0F;
  const float note_step = text->LineHeight() + 6.0F;
  for (std::size_t row = 0; row < kVisibleSceneRows; ++row) {
    const std::size_t index = first + row;
    if (index >= scenes.size()) {
      break;
    }
    const Scene& scene = scenes[index];
    const bool is_active = index == active;
    const bool is_selected = index == selected;
    const std::string line = std::to_string(index + 1) +
                             (is_active ? ". * " : ".   ") +
                             (is_selected ? "> " : "  ") + scene.name;
    text->Draw(line, left, cursor_y, is_selected ? kAuthorText : kLogText,
               content_width);
    cursor_y += name_step;
    if (!scene.note.empty()) {
      text->Draw("    " + scene.note, left, cursor_y, kHintText, content_width);
    }
    cursor_y += note_step;
  }
  const float bottom_y = y + height - 56.0F;
  if (!edit_label.empty()) {
    text->Draw(edit_label + edit_text + "_", left, bottom_y, kInputText,
               content_width);
    if (!status.empty()) {
      text->Draw(status, left, bottom_y + text->LineHeight() + 2.0F, kErrorText,
                 content_width);
    }
  } else if (!status.empty()) {
    text->Draw(status, left, bottom_y, kErrorText, content_width);
  } else {
    text->DrawWrapped(hint, left, bottom_y, kHintText, content_width);
  }
}

auto DrawConnectionBanner(SDL_Renderer* renderer, TextRenderer* text,
                          std::string_view message) -> void {
  if (renderer == nullptr || text == nullptr || message.empty()) {
    return;
  }
  int window_width = 0;
  int window_height = 0;
  if (!RenderSize(renderer, window_width, window_height)) {
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
  if (!RenderSize(renderer, window_width, window_height)) {
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

auto DrawExitToMenuPanel(SDL_Renderer* renderer, TextRenderer* text,
                         bool is_host) -> void {
  if (renderer == nullptr || text == nullptr) {
    return;
  }
  int window_width = 0;
  int window_height = 0;
  if (!RenderSize(renderer, window_width, window_height)) {
    return;
  }
  constexpr float padding = 24.0F;
  constexpr float gap = 10.0F;
  const float width =
      std::min(560.0F, static_cast<float>(window_width) - 40.0F);
  const float content_width = width - (2.0F * padding);
  const std::string_view message =
      is_host ? strings::kExitMenuHost : strings::kExitMenuClient;
  const float line_height = text->LineHeight();
  const float message_height = text->Measure(message, content_width).height;
  const float height =
      (2.0F * padding) + (2.0F * line_height) + message_height + (2.0F * gap);
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
  float top = y + padding;
  text->Draw(strings::kExitMenuTitle, left, top, kInputText, content_width);
  top += line_height + gap;
  top = text->DrawWrapped(message, left, top, kLogText, content_width);
  top += gap;
  text->Draw(strings::kExitMenuHint, left, top, kHintText, content_width);
}

}  // namespace dnd
