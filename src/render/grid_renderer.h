#pragma once

#include <cstdint>
#include <vector>

#include "core/grid_map.h"
#include "core/visibility.h"

struct SDL_Renderer;

namespace dnd {

class TextRenderer;
struct Token;

// Сетка, стены, токены и туман. offset_x/offset_y — позиция камеры в пикселях
// окна; cell_size уже учитывает зум.
auto DrawGrid(SDL_Renderer* renderer, const GridMap& map, float cell_size,
              float offset_x, float offset_y) -> void;
auto DrawWalls(SDL_Renderer* renderer, const GridMap& map, float cell_size,
               float offset_x, float offset_y) -> void;
auto DrawCellHighlight(SDL_Renderer* renderer, Cell cell, float cell_size,
                       float offset_x, float offset_y) -> void;
auto DrawTokens(SDL_Renderer* renderer, const std::vector<Token>& tokens,
                float cell_size, float offset_x, float offset_y,
                TextRenderer* text, std::uint32_t selected_id,
                std::uint32_t active_id) -> void;
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

}  // namespace dnd
