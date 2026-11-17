#pragma once

#include <cstdint>
#include <vector>

#include "core/grid_map.h"
#include "core/visibility.h"

struct SDL_Renderer;

namespace dnd {

class TextRenderer;
struct Token;

// Grid, walls, tokens and fog. offset_x/offset_y are the camera position in
// window pixels; cell_size is already zoomed.
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

}  // namespace dnd
