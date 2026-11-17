#include "render/grid_renderer.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <string>

#include "core/token.h"
#include "render/text_renderer.h"

namespace dnd {
namespace {

constexpr SDL_Color kBackground{
    .r = 0x18, .g = 0x18, .b = 0x20, .a = SDL_ALPHA_OPAQUE};
constexpr SDL_Color kGridLine{
    .r = 0x3a, .g = 0x3a, .b = 0x48, .a = SDL_ALPHA_OPAQUE};
constexpr SDL_Color kWallFill{
    .r = 0x52, .g = 0x52, .b = 0x5e, .a = SDL_ALPHA_OPAQUE};
constexpr SDL_Color kWallEdge{
    .r = 0x6e, .g = 0x6e, .b = 0x7c, .a = SDL_ALPHA_OPAQUE};
constexpr SDL_Color kTurnOutline{
    .r = 0x6c, .g = 0xa8, .b = 0xff, .a = SDL_ALPHA_OPAQUE};
constexpr SDL_Color kSelectedOutline{
    .r = 0xf5, .g = 0xe0, .b = 0x8c, .a = SDL_ALPHA_OPAQUE};
constexpr SDL_Color kHpBarBack{.r = 0x20, .g = 0x20, .b = 0x26, .a = 0xcc};
constexpr SDL_Color kHpHealthy{
    .r = 0x50, .g = 0xc8, .b = 0x64, .a = SDL_ALPHA_OPAQUE};
constexpr SDL_Color kHpWounded{
    .r = 0xd8, .g = 0xb0, .b = 0x40, .a = SDL_ALPHA_OPAQUE};
constexpr SDL_Color kHpCritical{
    .r = 0xd8, .g = 0x50, .b = 0x50, .a = SDL_ALPHA_OPAQUE};
constexpr SDL_Color kTokenOutline{.r = 0x10, .g = 0x10, .b = 0x14, .a = 0xaa};
constexpr SDL_Color kLabel{
    .r = 0xf5, .g = 0xf5, .b = 0xf5, .a = SDL_ALPHA_OPAQUE};
constexpr SDL_Color kDropHighlight{.r = 0x6c, .g = 0xa8, .b = 0xff, .a = 0x50};
constexpr SDL_Color kDropBorder{.r = 0x9c, .g = 0xc8, .b = 0xff, .a = 0xff};
constexpr SDL_Color kFogUnknown{
    .r = 0x08, .g = 0x08, .b = 0x0a, .a = SDL_ALPHA_OPAQUE};
constexpr SDL_Color kFogExplored{.r = 0x08, .g = 0x08, .b = 0x0a, .a = 0xc0};

constexpr std::array<SDL_Color, 4> kTokenColors{{
    {.r = 0xc8, .g = 0x50, .b = 0x46, .a = SDL_ALPHA_OPAQUE},
    {.r = 0x46, .g = 0xa8, .b = 0x64, .a = SDL_ALPHA_OPAQUE},
    {.r = 0x50, .g = 0x7a, .b = 0xc8, .a = SDL_ALPHA_OPAQUE},
    {.r = 0xc8, .g = 0xa8, .b = 0x3c, .a = SDL_ALPHA_OPAQUE},
}};

constexpr float kTokenInset = 4.0F;
constexpr float kWallInset = 1.0F;
constexpr float kWallEdgeWidth = 3.0F;

auto SetDrawColor(SDL_Renderer* renderer, SDL_Color color) -> void {
  SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
}

auto CellRect(Cell cell, float cell_size, float offset_x, float offset_y,
              float inset) -> SDL_FRect {
  return SDL_FRect{
      .x = offset_x + (static_cast<float>(cell.x) * cell_size) + inset,
      .y = offset_y + (static_cast<float>(cell.y) * cell_size) + inset,
      .w = cell_size - (2.0F * inset),
      .h = cell_size - (2.0F * inset),
  };
}

auto TokenColor(const Token& token) -> SDL_Color {
  if (token.color != 0) {
    return SDL_Color{
        .r = static_cast<std::uint8_t>((token.color >> 16) & 0xFFU),
        .g = static_cast<std::uint8_t>((token.color >> 8) & 0xFFU),
        .b = static_cast<std::uint8_t>(token.color & 0xFFU),
        .a = SDL_ALPHA_OPAQUE,
    };
  }
  if (token.id == 0) {
    return kTokenColors.front();
  }
  const std::size_t index =
      static_cast<std::size_t>(token.id - 1) % kTokenColors.size();
  return kTokenColors[index];
}

}  // namespace

auto DrawGrid(SDL_Renderer* renderer, const GridMap& map, float cell_size,
              float offset_x, float offset_y) -> void {
  SetDrawColor(renderer, kBackground);
  SDL_RenderClear(renderer);

  SetDrawColor(renderer, kGridLine);
  const auto width = static_cast<float>(map.Width()) * cell_size;
  const auto height = static_cast<float>(map.Height()) * cell_size;
  for (int x = 0; x <= map.Width(); ++x) {
    const auto px = offset_x + (static_cast<float>(x) * cell_size);
    SDL_RenderLine(renderer, px, offset_y, px, offset_y + height);
  }
  for (int y = 0; y <= map.Height(); ++y) {
    const auto py = offset_y + (static_cast<float>(y) * cell_size);
    SDL_RenderLine(renderer, offset_x, py, offset_x + width, py);
  }
}

auto DrawWalls(SDL_Renderer* renderer, const GridMap& map, float cell_size,
               float offset_x, float offset_y) -> void {
  for (int y = 0; y < map.Height(); ++y) {
    for (int x = 0; x < map.Width(); ++x) {
      const Cell cell{.x = x, .y = y};
      if (!map.IsBlocked(cell)) {
        continue;
      }
      const SDL_FRect rect =
          CellRect(cell, cell_size, offset_x, offset_y, kWallInset);
      SetDrawColor(renderer, kWallFill);
      SDL_RenderFillRect(renderer, &rect);
      const SDL_FRect inner{
          .x = rect.x + kWallEdgeWidth,
          .y = rect.y + kWallEdgeWidth,
          .w = rect.w - (2.0F * kWallEdgeWidth),
          .h = rect.h - (2.0F * kWallEdgeWidth),
      };
      SetDrawColor(renderer, kWallEdge);
      if (inner.w > 0.0F && inner.h > 0.0F) {
        SDL_RenderRect(renderer, &inner);
      }
    }
  }
}

auto DrawCellHighlight(SDL_Renderer* renderer, Cell cell, float cell_size,
                       float offset_x, float offset_y) -> void {
  const SDL_FRect rect = CellRect(cell, cell_size, offset_x, offset_y, 0.0F);
  SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
  SetDrawColor(renderer, kDropHighlight);
  SDL_RenderFillRect(renderer, &rect);
  SetDrawColor(renderer, kDropBorder);
  SDL_RenderRect(renderer, &rect);
  SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
}

auto DrawTokens(SDL_Renderer* renderer, const std::vector<Token>& tokens,
                float cell_size, float offset_x, float offset_y,
                TextRenderer* text, std::uint32_t selected_id,
                std::uint32_t active_id) -> void {
  for (const Token& token : tokens) {
    const SDL_FRect rect =
        CellRect(token.cell, cell_size, offset_x, offset_y, kTokenInset);
    SetDrawColor(renderer, TokenColor(token));
    SDL_RenderFillRect(renderer, &rect);

    const bool selected = token.id == selected_id;
    SetDrawColor(renderer, selected ? kSelectedOutline : kTokenOutline);
    SDL_RenderRect(renderer, &rect);
    if (selected) {
      const SDL_FRect inner{
          .x = rect.x + 2.0F,
          .y = rect.y + 2.0F,
          .w = rect.w - 4.0F,
          .h = rect.h - 4.0F,
      };
      SDL_RenderRect(renderer, &inner);
    }

    if (active_id != 0 && token.id == active_id) {
      const SDL_FRect turn{
          .x = rect.x - 1.0F,
          .y = rect.y - 1.0F,
          .w = rect.w + 2.0F,
          .h = rect.h + 2.0F,
      };
      SetDrawColor(renderer, kTurnOutline);
      SDL_RenderRect(renderer, &turn);
      SDL_RenderRect(renderer, &rect);
    }

    if (token.max_hp > 0) {
      const float ratio = std::clamp(
          static_cast<float>(token.hp) / static_cast<float>(token.max_hp), 0.0F,
          1.0F);
      const SDL_FRect bar_back{
          .x = rect.x + 3.0F,
          .y = (rect.y + rect.h) - 7.0F,
          .w = rect.w - 6.0F,
          .h = 4.0F,
      };
      SetDrawColor(renderer, kHpBarBack);
      SDL_RenderFillRect(renderer, &bar_back);

      SDL_Color hp_color = kHpHealthy;
      if (ratio <= 0.25F) {
        hp_color = kHpCritical;
      } else if (ratio <= 0.5F) {
        hp_color = kHpWounded;
      }
      const SDL_FRect bar{
          .x = bar_back.x,
          .y = bar_back.y,
          .w = bar_back.w * ratio,
          .h = bar_back.h,
      };
      SetDrawColor(renderer, hp_color);
      SDL_RenderFillRect(renderer, &bar);
    }

    if (text != nullptr) {
      text->DrawCentered(token.name, rect, kLabel);
    }
  }
}

auto DrawFog(SDL_Renderer* renderer, const FogOfWar& fog, float cell_size,
             float offset_x, float offset_y) -> void {
  SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
  for (int y = 0; y < fog.Height(); ++y) {
    for (int x = 0; x < fog.Width(); ++x) {
      const Cell cell{.x = x, .y = y};
      if (fog.IsVisible(cell)) {
        continue;
      }
      const SDL_FRect rect =
          CellRect(cell, cell_size, offset_x, offset_y, 0.0F);
      SetDrawColor(renderer, fog.IsExplored(cell) ? kFogExplored : kFogUnknown);
      SDL_RenderFillRect(renderer, &rect);
    }
  }
  SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
}

}  // namespace dnd
