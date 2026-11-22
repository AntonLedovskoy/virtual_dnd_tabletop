#include "render/editor_renderer.h"

#include <string>
#include <string_view>

#include "app/strings.h"
#include "render/text_renderer.h"

namespace dnd {
namespace {

constexpr SDL_Color kPanelBackground{
    .r = 0x0c, .g = 0x0c, .b = 0x12, .a = 0xd0};
constexpr SDL_Color kPanelBorder{
    .r = 0x3a, .g = 0x3a, .b = 0x48, .a = SDL_ALPHA_OPAQUE};
constexpr SDL_Color kTitleText{
    .r = 0x9c, .g = 0xc8, .b = 0xff, .a = SDL_ALPHA_OPAQUE};
constexpr SDL_Color kText{
    .r = 0xdc, .g = 0xdc, .b = 0xe4, .a = SDL_ALPHA_OPAQUE};
constexpr SDL_Color kHint{
    .r = 0x80, .g = 0x80, .b = 0x90, .a = SDL_ALPHA_OPAQUE};

constexpr int kPanelX = 12;
constexpr int kPanelY = 12;
constexpr int kPanelWidth = 350;
constexpr int kPanelHeight = 520;
constexpr int kPadding = 12;

auto SetDrawColor(SDL_Renderer* renderer, SDL_Color color) -> void {
  SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
}

auto BrushLabel(EditorBrush brush) -> std::string_view {
  switch (brush) {
    case EditorBrush::kWall:
      return strings::kBrushWall;
    case EditorBrush::kFloor:
      return strings::kBrushFloor;
    case EditorBrush::kTree:
      return strings::kBrushTree;
    case EditorBrush::kWater:
      return strings::kBrushWater;
    case EditorBrush::kPit:
      return strings::kBrushPit;
    case EditorBrush::kLava:
      return strings::kBrushLava;
  }
  return strings::kBrushWall;
}

}  // namespace

auto EditorPanelBounds() -> SDL_FRect {
  return SDL_FRect{.x = static_cast<float>(kPanelX),
                   .y = static_cast<float>(kPanelY),
                   .w = static_cast<float>(kPanelWidth),
                   .h = static_cast<float>(kPanelHeight)};
}

auto DrawEditorPanel(SDL_Renderer* renderer, TextRenderer* text,
                     const GameSnapshot& scene, EditorBrush brush,
                     std::size_t scene_index, std::size_t scene_count,
                     const std::string& scene_name, const std::string& status,
                     std::uint32_t selected_id, const std::string& edit_label,
                     const std::string& edit_text) -> void {
  if (renderer == nullptr || text == nullptr) {
    return;
  }
  const SDL_FRect panel = EditorPanelBounds();
  SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
  SetDrawColor(renderer, kPanelBackground);
  SDL_RenderFillRect(renderer, &panel);
  SetDrawColor(renderer, kPanelBorder);
  SDL_RenderRect(renderer, &panel);
  SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);

  const auto text_x = static_cast<float>(kPanelX + kPadding);
  const auto max_text_width = static_cast<float>(kPanelWidth - (2 * kPadding));
  const int line_height = static_cast<int>(text->LineHeight()) + 4;
  auto y = static_cast<float>(kPanelY + kPadding);

  text->Draw(std::string(strings::kEditorTitle), text_x, y, kTitleText,
             max_text_width);
  y += static_cast<float>(line_height);
  const std::string map_name =
      scene_name.empty() ? std::string(strings::kEditorNoMap) : scene_name;
  std::string map_line = std::string(strings::kEditorMap) + ": " + map_name;
  if (scene_count > 0) {
    map_line += "   " + std::to_string(scene_index + 1) + " " +
                std::string(strings::kEditorMapOf) + " " +
                std::to_string(scene_count);
  }
  text->Draw(map_line, text_x, y, kTitleText, max_text_width);
  y += static_cast<float>(line_height);
  y = text->DrawWrapped(std::string(strings::kEditorDescription), text_x, y,
                        kHint, max_text_width) +
      8.0F;

  const std::string size = std::string(strings::kEditorSize) + ": " +
                           std::to_string(scene.map.Width()) + " x " +
                           std::to_string(scene.map.Height());
  text->Draw(size, text_x, y, kText, max_text_width);
  y += static_cast<float>(line_height);
  text->Draw(std::string(strings::kEditorSizeHint), text_x, y, kHint,
             max_text_width);
  y += static_cast<float>(line_height);

  std::string token_line = "Токенов: " + std::to_string(scene.tokens.size());
  for (const Token& token : scene.tokens) {
    if (token.id == selected_id) {
      token_line += "   выбран: " + token.name;
      break;
    }
  }
  text->Draw(token_line, text_x, y, kText, max_text_width);
  y += static_cast<float>(line_height);

  const std::string brush_line = std::string(strings::kEditorBrush) + ": " +
                                 std::string(BrushLabel(brush));
  text->Draw(brush_line, text_x, y, kTitleText, max_text_width);
  y += static_cast<float>(line_height) + 8.0F;

  y = text->DrawWrapped(std::string(strings::kEditorMouseHint), text_x, y,
                        kHint, max_text_width) +
      6.0F;
  y = text->DrawWrapped(std::string(strings::kEditorKeyHint), text_x, y, kHint,
                        max_text_width) +
      6.0F;

  if (!edit_label.empty()) {
    y = text->DrawWrapped(edit_label + edit_text + "_", text_x, y, kTitleText,
                          max_text_width) +
        4.0F;
    if (!status.empty()) {
      text->DrawWrapped(status, text_x, y, kHint, max_text_width);
    }
  } else if (!status.empty()) {
    text->DrawWrapped(status, text_x, y, kTitleText, max_text_width);
  }
}

}  // namespace dnd
