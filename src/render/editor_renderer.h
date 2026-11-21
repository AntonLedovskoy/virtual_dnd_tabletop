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
  kTree = 2,
  kWater = 3,
  kPit = 4,
  kLava = 5,
};

// Фиксированная панель слева сверху. Клики внутри неё зарезервированы для
// будущих элементов и не должны рисовать на карте под панелью.
auto EditorPanelBounds() -> SDL_FRect;

auto DrawEditorPanel(SDL_Renderer* renderer, TextRenderer* text,
                     const GameSnapshot& scene, EditorBrush brush,
                     const std::string& status) -> void;

}  // namespace dnd
