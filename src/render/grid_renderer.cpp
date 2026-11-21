#include "render/grid_renderer.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <string>

#include "core/client_view.h"
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
constexpr SDL_Color kTreeCanopy{
    .r = 0x2e, .g = 0x6b, .b = 0x3a, .a = SDL_ALPHA_OPAQUE};
constexpr SDL_Color kTreeTrunk{
    .r = 0x5a, .g = 0x3c, .b = 0x22, .a = SDL_ALPHA_OPAQUE};
constexpr SDL_Color kWaterFill{
    .r = 0x1f, .g = 0x4f, .b = 0x7a, .a = SDL_ALPHA_OPAQUE};
constexpr SDL_Color kWaterWave{
    .r = 0x4f, .g = 0x92, .b = 0xc8, .a = SDL_ALPHA_OPAQUE};
constexpr SDL_Color kPitFill{
    .r = 0x0a, .g = 0x0a, .b = 0x0e, .a = SDL_ALPHA_OPAQUE};
constexpr SDL_Color kPitEdge{
    .r = 0x50, .g = 0x50, .b = 0x5e, .a = SDL_ALPHA_OPAQUE};
constexpr SDL_Color kLavaFill{
    .r = 0xb8, .g = 0x38, .b = 0x14, .a = SDL_ALPHA_OPAQUE};
constexpr SDL_Color kLavaEdge{
    .r = 0xff, .g = 0x9a, .b = 0x3c, .a = SDL_ALPHA_OPAQUE};
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
constexpr SDL_Color kMoveFill{.r = 0x6c, .g = 0xa8, .b = 0xff, .a = 0x40};
constexpr SDL_Color kMoveBorder{.r = 0x9c, .g = 0xc8, .b = 0xff, .a = 0xb0};
constexpr SDL_Color kRuler{.r = 0xf5, .g = 0xe0, .b = 0x8c, .a = 0xff};
constexpr SDL_Color kAoeFill{.r = 0xff, .g = 0x6b, .b = 0x4a, .a = 0x48};
constexpr SDL_Color kAoeBorder{.r = 0xff, .g = 0x9a, .b = 0x7a, .a = 0xc0};
constexpr SDL_Color kPing{.r = 0xff, .g = 0xe0, .b = 0x5a, .a = 0xff};

constexpr std::array<SDL_Color, 4> kTokenColors{{
    {.r = 0xc8, .g = 0x50, .b = 0x46, .a = SDL_ALPHA_OPAQUE},
    {.r = 0x46, .g = 0xa8, .b = 0x64, .a = SDL_ALPHA_OPAQUE},
    {.r = 0x50, .g = 0x7a, .b = 0xc8, .a = SDL_ALPHA_OPAQUE},
    {.r = 0xc8, .g = 0xa8, .b = 0x3c, .a = SDL_ALPHA_OPAQUE},
}};

constexpr float kTokenInset = 4.0F;
constexpr float kWallInset = 1.0F;
constexpr float kWallEdgeWidth = 3.0F;
// Круг токена строится веером треугольников: SDL не умеет рисовать круги сам.
constexpr int kCircleSegments = 48;
constexpr std::size_t kCircleIndexCount =
    static_cast<std::size_t>(kCircleSegments) * 3;
constexpr float kFullTurn = 6.28318530718F;

auto SetDrawColor(SDL_Renderer* renderer, SDL_Color color) -> void {
  SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
}

auto ToFColor(SDL_Color color) -> SDL_FColor {
  return SDL_FColor{
      .r = static_cast<float>(color.r) / 255.0F,
      .g = static_cast<float>(color.g) / 255.0F,
      .b = static_cast<float>(color.b) / 255.0F,
      .a = static_cast<float>(color.a) / 255.0F,
  };
}

auto BuildCircle(SDL_FPoint center, float radius, SDL_Color color,
                 std::array<SDL_Vertex, kCircleSegments + 1>& vertices)
    -> void {
  const SDL_FPoint middle{.x = 0.5F, .y = 0.5F};
  const SDL_FColor vertex_color = ToFColor(color);
  vertices[0] = SDL_Vertex{
      .position = center, .color = vertex_color, .tex_coord = middle};
  for (int i = 0; i < kCircleSegments; ++i) {
    const float angle =
        kFullTurn * static_cast<float>(i) / static_cast<float>(kCircleSegments);
    const float cosine = std::cos(angle);
    const float sine = std::sin(angle);
    vertices[static_cast<std::size_t>(i) + 1] =
        SDL_Vertex{.position = SDL_FPoint{.x = center.x + (radius * cosine),
                                          .y = center.y + (radius * sine)},
                   .color = vertex_color,
                   .tex_coord = SDL_FPoint{.x = middle.x + (cosine * 0.5F),
                                           .y = middle.y + (sine * 0.5F)}};
  }
}

auto CircleIndices() -> const std::array<int, kCircleIndexCount>& {
  static const std::array<int, kCircleIndexCount> indices =
      []() -> std::array<int, kCircleIndexCount> {
    std::array<int, kCircleIndexCount> built{};
    for (int i = 0; i < kCircleSegments; ++i) {
      const auto base = static_cast<std::size_t>(i) * 3;
      built[base] = 0;
      built[base + 1] = i + 1;
      built[base + 2] = ((i + 1) % kCircleSegments) + 1;
    }
    return built;
  }();
  return indices;
}

auto DrawCircle(SDL_Renderer* renderer, SDL_FPoint center, float radius,
                SDL_Color color) -> void {
  std::array<SDL_Vertex, kCircleSegments + 1> vertices{};
  BuildCircle(center, radius, color, vertices);
  const auto& indices = CircleIndices();
  SDL_RenderGeometry(renderer, nullptr, vertices.data(),
                     static_cast<int>(vertices.size()), indices.data(),
                     static_cast<int>(indices.size()));
}

auto DrawTexturedCircle(SDL_Renderer* renderer, SDL_Texture* texture,
                        SDL_FPoint center, float radius) -> void {
  constexpr SDL_Color white{
      .r = 0xff, .g = 0xff, .b = 0xff, .a = SDL_ALPHA_OPAQUE};
  std::array<SDL_Vertex, kCircleSegments + 1> vertices{};
  BuildCircle(center, radius, white, vertices);
  const auto& indices = CircleIndices();
  SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
  SDL_RenderGeometry(renderer, texture, vertices.data(),
                     static_cast<int>(vertices.size()), indices.data(),
                     static_cast<int>(indices.size()));
}

auto CellCenter(Cell cell, float cell_size, float offset_x, float offset_y)
    -> SDL_FPoint {
  return SDL_FPoint{
      .x = offset_x + ((static_cast<float>(cell.x) + 0.5F) * cell_size),
      .y = offset_y + ((static_cast<float>(cell.y) + 0.5F) * cell_size),
  };
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

auto ClearBoard(SDL_Renderer* renderer) -> void {
  SetDrawColor(renderer, kBackground);
  SDL_RenderClear(renderer);
}

auto DrawMapBackground(SDL_Renderer* renderer, SDL_Texture* texture,
                       const GridMap& map, int opacity, int scale,
                       float cell_size, float offset_x, float offset_y)
    -> void {
  if (texture == nullptr || opacity <= 0) {
    return;
  }
  float texture_width = 0.0F;
  float texture_height = 0.0F;
  if (!SDL_GetTextureSize(texture, &texture_width, &texture_height) ||
      texture_width <= 0.0F || texture_height <= 0.0F) {
    return;
  }
  const float map_width = static_cast<float>(map.Width()) * cell_size;
  const float map_height = static_cast<float>(map.Height()) * cell_size;
  // Вписываем картинку в карту целиком, затем применяем масштаб. Пропорции
  // сохраняются, картинка центрируется.
  const float fit =
      std::min(map_width / texture_width, map_height / texture_height) *
      (static_cast<float>(scale) / 100.0F);
  const SDL_FRect destination{
      .x = offset_x + ((map_width - (texture_width * fit)) / 2.0F),
      .y = offset_y + ((map_height - (texture_height * fit)) / 2.0F),
      .w = texture_width * fit,
      .h = texture_height * fit,
  };
  const SDL_Rect clip{
      .x = static_cast<int>(offset_x),
      .y = static_cast<int>(offset_y),
      .w = static_cast<int>(map_width),
      .h = static_cast<int>(map_height),
  };
  SDL_SetRenderClipRect(renderer, &clip);
  SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
  const auto alpha =
      static_cast<std::uint8_t>((opacity * SDL_ALPHA_OPAQUE + 50) / 100);
  SDL_SetTextureAlphaMod(texture, alpha);
  SDL_RenderTexture(renderer, texture, nullptr, &destination);
  SDL_SetTextureAlphaMod(texture, SDL_ALPHA_OPAQUE);
  SDL_SetRenderClipRect(renderer, nullptr);
}

auto DrawGrid(SDL_Renderer* renderer, const GridMap& map, float cell_size,
              float offset_x, float offset_y) -> void {
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

auto DrawTerrain(SDL_Renderer* renderer, const GridMap& map, float cell_size,
                 float offset_x, float offset_y) -> void {
  for (int y = 0; y < map.Height(); ++y) {
    for (int x = 0; x < map.Width(); ++x) {
      const Cell cell{.x = x, .y = y};
      const Terrain terrain = map.TerrainAt(cell);
      const SDL_FRect rect =
          CellRect(cell, cell_size, offset_x, offset_y, kWallInset);
      switch (terrain) {
        case Terrain::kFloor:
          break;
        case Terrain::kWall: {
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
          break;
        }
        case Terrain::kTree: {
          // Ствол у нижнего края и крона: клетка проходима, но закрывает
          // обзор на две клетки за собой.
          const SDL_FRect trunk{
              .x = rect.x + (rect.w * 0.44F),
              .y = rect.y + (rect.h * 0.58F),
              .w = rect.w * 0.12F,
              .h = rect.h * 0.36F,
          };
          SetDrawColor(renderer, kTreeTrunk);
          SDL_RenderFillRect(renderer, &trunk);
          const SDL_FPoint center{
              .x = rect.x + (rect.w * 0.5F),
              .y = rect.y + (rect.h * 0.42F),
          };
          DrawCircle(renderer, center, rect.w * 0.34F, kTreeCanopy);
          break;
        }
        case Terrain::kWater: {
          SetDrawColor(renderer, kWaterFill);
          SDL_RenderFillRect(renderer, &rect);
          SetDrawColor(renderer, kWaterWave);
          for (int line = 1; line <= 2; ++line) {
            const float y_line =
                rect.y + (rect.h * (static_cast<float>(line) * 0.3F));
            SDL_RenderLine(renderer, rect.x + (rect.w * 0.15F), y_line,
                           rect.x + (rect.w * 0.85F), y_line);
          }
          break;
        }
        case Terrain::kPit: {
          SetDrawColor(renderer, kPitFill);
          SDL_RenderFillRect(renderer, &rect);
          const SDL_FRect edge{
              .x = rect.x + (rect.w * 0.12F),
              .y = rect.y + (rect.h * 0.12F),
              .w = rect.w * 0.76F,
              .h = rect.h * 0.76F,
          };
          SetDrawColor(renderer, kPitEdge);
          SDL_RenderRect(renderer, &edge);
          break;
        }
        case Terrain::kLava: {
          SetDrawColor(renderer, kLavaFill);
          SDL_RenderFillRect(renderer, &rect);
          SetDrawColor(renderer, kLavaEdge);
          const SDL_FRect crack{
              .x = rect.x + (rect.w * 0.28F),
              .y = rect.y + (rect.h * 0.28F),
              .w = rect.w * 0.2F,
              .h = rect.h * 0.12F,
          };
          SDL_RenderFillRect(renderer, &crack);
          const SDL_FRect crack2{
              .x = rect.x + (rect.w * 0.58F),
              .y = rect.y + (rect.h * 0.62F),
              .w = rect.w * 0.18F,
              .h = rect.h * 0.12F,
          };
          SDL_RenderFillRect(renderer, &crack2);
          break;
        }
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
                std::uint32_t active_id, const std::vector<TokenArt>* art)
    -> void {
  for (std::size_t index = 0; index < tokens.size(); ++index) {
    const Token& token = tokens[index];
    const SDL_FRect rect =
        CellRect(token.cell, cell_size, offset_x, offset_y, kTokenInset);
    const SDL_FPoint center =
        CellCenter(token.cell, cell_size, offset_x, offset_y);
    const float radius = rect.w / 2.0F;

    const bool selected = token.id == selected_id;
    const bool active = active_id != 0 && token.id == active_id;
    if (active) {
      DrawCircle(renderer, center, radius + 4.0F, kTurnOutline);
    }
    if (selected) {
      DrawCircle(renderer, center, radius + 2.0F, kSelectedOutline);
    }
    DrawCircle(renderer, center, radius + 1.0F, kTokenOutline);
    DrawCircle(renderer, center, radius, TokenColor(token));
    if (art != nullptr && index < art->size() &&
        (*art)[index].portrait != nullptr) {
      DrawTexturedCircle(renderer, (*art)[index].portrait, center, radius);
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

auto DrawMovementRange(SDL_Renderer* renderer, const std::vector<Cell>& cells,
                       float cell_size, float offset_x, float offset_y)
    -> void {
  SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
  for (const Cell cell : cells) {
    const SDL_FRect rect = CellRect(cell, cell_size, offset_x, offset_y, 1.0F);
    SetDrawColor(renderer, kMoveFill);
    SDL_RenderFillRect(renderer, &rect);
    SetDrawColor(renderer, kMoveBorder);
    SDL_RenderRect(renderer, &rect);
  }
  SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
}

auto DrawRuler(SDL_Renderer* renderer, Cell from, Cell to, float cell_size,
               float offset_x, float offset_y) -> void {
  const SDL_FPoint start = CellCenter(from, cell_size, offset_x, offset_y);
  const SDL_FPoint end = CellCenter(to, cell_size, offset_x, offset_y);
  SetDrawColor(renderer, kRuler);
  SDL_RenderLine(renderer, start.x, start.y, end.x, end.y);
  constexpr float radius = 4.0F;
  const SDL_FRect start_dot{.x = start.x - radius,
                            .y = start.y - radius,
                            .w = radius * 2.0F,
                            .h = radius * 2.0F};
  const SDL_FRect end_dot{.x = end.x - radius,
                          .y = end.y - radius,
                          .w = radius * 2.0F,
                          .h = radius * 2.0F};
  SDL_RenderFillRect(renderer, &start_dot);
  SDL_RenderFillRect(renderer, &end_dot);
}

auto DrawAoe(SDL_Renderer* renderer, const std::vector<Cell>& cells,
             float cell_size, float offset_x, float offset_y) -> void {
  SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
  for (const Cell cell : cells) {
    const SDL_FRect rect = CellRect(cell, cell_size, offset_x, offset_y, 1.0F);
    SetDrawColor(renderer, kAoeFill);
    SDL_RenderFillRect(renderer, &rect);
    SetDrawColor(renderer, kAoeBorder);
    SDL_RenderRect(renderer, &rect);
  }
  SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
}

auto DrawPings(SDL_Renderer* renderer, const std::vector<MapPing>& pings,
               std::uint32_t now, float cell_size, float offset_x,
               float offset_y, TextRenderer* text) -> void {
  if (renderer == nullptr) {
    return;
  }
  SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
  for (const MapPing& ping : pings) {
    if (now < ping.created_at || now - ping.created_at >= kPingSeconds) {
      continue;
    }
    const auto age = static_cast<float>(now - ping.created_at);
    // Метка гаснет к концу жизни, но не исчезает скачком.
    const float fade = std::clamp(
        1.0F - (age / static_cast<float>(kPingSeconds)), 0.35F, 1.0F);
    SDL_Color color = kPing;
    color.a = static_cast<std::uint8_t>(fade * 255.0F);
    const SDL_FRect outer =
        CellRect(ping.cell, cell_size, offset_x, offset_y, 2.0F);
    SetDrawColor(renderer, color);
    SDL_RenderRect(renderer, &outer);
    const SDL_FRect inner =
        CellRect(ping.cell, cell_size, offset_x, offset_y, cell_size * 0.3F);
    SDL_RenderRect(renderer, &inner);
    const SDL_FPoint center =
        CellCenter(ping.cell, cell_size, offset_x, offset_y);
    const float arm = cell_size * 0.2F;
    SDL_RenderLine(renderer, center.x - arm, center.y, center.x + arm,
                   center.y);
    SDL_RenderLine(renderer, center.x, center.y - arm, center.x,
                   center.y + arm);
    if (text != nullptr && !ping.author.empty()) {
      text->Draw(ping.author, outer.x, outer.y - 18.0F, kPing,
                 cell_size * 3.0F);
    }
  }
  SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
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
