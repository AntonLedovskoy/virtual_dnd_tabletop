#pragma once

#include <SDL3/SDL.h>

#include <cstdint>
#include <string>

#include "core/snapshot.h"

namespace dnd {

class TextRenderer;

enum class EditorBrush : std::uint8_t {
  kWall = 0,
  kFloor = 1,
  kPlayer = 2,
  kMonster = 3,
  kErase = 4,
};

// Fixed top-left panel. Clicks inside it are reserved for future controls and
// must not paint the map underneath.
auto EditorPanelBounds() -> SDL_FRect;

auto DrawEditorPanel(SDL_Renderer* renderer, TextRenderer* text,
                     const GameSnapshot& scene, EditorBrush brush,
                     const std::string& status) -> void;

}  // namespace dnd
