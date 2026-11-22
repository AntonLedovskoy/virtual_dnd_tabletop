#pragma once

#include <SDL3/SDL.h>

#include <cstddef>
#include <cstdint>
#include <string>

#include "core/snapshot.h"

namespace dnd {

class TextRenderer;

enum class EditorBrush : std::uint8_t {
  kWall = 0,
  kFloor = 1,
  kTree = 2,
  kWater = 3,
  kPit = 4,
  kLava = 5,
};

// Фиксированная панель слева сверху: имя открытой карты, «N из M» и кисти.
auto EditorPanelBounds() -> SDL_FRect;

auto DrawEditorPanel(SDL_Renderer* renderer, TextRenderer* text,
                     const GameSnapshot& scene, EditorBrush brush,
                     std::size_t scene_index, std::size_t scene_count,
                     const std::string& scene_name, const std::string& status)
    -> void;

}  // namespace dnd
