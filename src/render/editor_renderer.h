#pragma once

#include <SDL3/SDL.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/grid_map.h"
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
  kRoad = 6,
  // Трудная местность: 8 циклически переключает эти кисти.
  kSwamp = 7,
  kBrush = 8,
  kSnow = 9,
  kMud = 10,
  // Свет и затемнение: 9 — свет клетки по кругу, Shift+9 — затемнение,
  // 0 — вернуть клетке свет сцены.
  kLight = 11,
  kObscurement = 12,
  kLightClear = 13,
};

// Фиксированная панель слева сверху: имя открытой карты, «N из M» и кисти.
auto EditorPanelBounds() -> SDL_FRect;

// selected_id — выбранный токен: его имя показывается в счётчике, edit_label
// и edit_text — строка переименования вместо статуса.
auto DrawEditorPanel(SDL_Renderer* renderer, TextRenderer* text,
                     const GameSnapshot& scene, EditorBrush brush,
                     std::size_t scene_index, std::size_t scene_count,
                     const std::string& scene_name, const std::string& status,
                     std::uint32_t selected_id, const std::string& edit_label,
                     const std::string& edit_text) -> void;

// Инспектор клетки под курсором: строки «поле: значение», активная строка
// подсвечена. Выбор поля и правка значений — в App.
struct EditorInspectorView {
  Cell cell{};
  std::vector<std::pair<std::string, std::string>> fields;
  int active = 0;
};

auto DrawEditorInspector(SDL_Renderer* renderer, TextRenderer* text,
                         const EditorInspectorView& view) -> void;

}  // namespace dnd
