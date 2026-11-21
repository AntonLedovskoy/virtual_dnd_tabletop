#pragma once

#include <SDL3/SDL.h>

#include <cstdint>
#include <vector>

#include "core/client_view.h"
#include "core/grid_map.h"
#include "core/visibility.h"

namespace dnd {

class TextRenderer;
struct Token;

// Сетка, рельеф, токены и туман. offset_x/offset_y — позиция камеры в пикселях
// окна; cell_size уже учитывает зум.
// Подложка карты рисуется между фоном и сеткой: картинка из файла рядом
// с .exe, вписанная в карту и растянутая на scale процентов.
auto ClearBoard(SDL_Renderer* renderer) -> void;
auto DrawMapBackground(SDL_Renderer* renderer, SDL_Texture* texture,
                       const GridMap& map, int opacity, int scale,
                       float cell_size, float offset_x, float offset_y) -> void;
auto DrawGrid(SDL_Renderer* renderer, const GridMap& map, float cell_size,
              float offset_x, float offset_y) -> void;
// Стены, деревья, вода, пропасти и лава.
auto DrawTerrain(SDL_Renderer* renderer, const GridMap& map, float cell_size,
                 float offset_x, float offset_y) -> void;
auto DrawCellHighlight(SDL_Renderer* renderer, Cell cell, float cell_size,
                       float offset_x, float offset_y) -> void;
// Картинка токена, уже загруженная приложением; nullptr — цвет и имя.
struct TokenArt {
  SDL_Texture* portrait = nullptr;
};

auto DrawTokens(SDL_Renderer* renderer, const std::vector<Token>& tokens,
                float cell_size, float offset_x, float offset_y,
                TextRenderer* text, std::uint32_t selected_id,
                std::uint32_t active_id,
                const std::vector<TokenArt>* art = nullptr) -> void;
auto DrawFog(SDL_Renderer* renderer, const FogOfWar& fog, float cell_size,
             float offset_x, float offset_y) -> void;

// Подсветка клеток, доступных для перемещения.
auto DrawMovementRange(SDL_Renderer* renderer, const std::vector<Cell>& cells,
                       float cell_size, float offset_x, float offset_y) -> void;

// Линия измерения между двумя клетками.
auto DrawRuler(SDL_Renderer* renderer, Cell from, Cell to, float cell_size,
               float offset_x, float offset_y) -> void;

// Шаблон области, который DM показывает столу.
auto DrawAoe(SDL_Renderer* renderer, const std::vector<Cell>& cells,
             float cell_size, float offset_x, float offset_y) -> void;

// Метки «сюда»: гаснут сами через kPingSeconds от created_at.
auto DrawPings(SDL_Renderer* renderer, const std::vector<MapPing>& pings,
               std::uint32_t now, float cell_size, float offset_x,
               float offset_y, TextRenderer* text) -> void;

}  // namespace dnd
