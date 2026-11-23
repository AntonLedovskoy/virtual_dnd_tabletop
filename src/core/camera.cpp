#include "core/camera.h"

#include <algorithm>
#include <cmath>

#include "core/player_name.h"

namespace dnd {

auto CenteredCamera(float view_width, float view_height, int map_width,
                    int map_height, float cell_size) -> Camera {
  Camera camera{.cell_size = cell_size};
  camera.x = (view_width - static_cast<float>(map_width) * cell_size) / 2.0F;
  camera.y = (view_height - static_cast<float>(map_height) * cell_size) / 2.0F;
  return camera;
}

auto CameraOnCell(float view_width, float view_height, int map_width,
                  int map_height, Cell cell, float cell_size) -> Camera {
  Camera camera{.cell_size = cell_size};
  const float map_width_px = static_cast<float>(map_width) * cell_size;
  const float map_height_px = static_cast<float>(map_height) * cell_size;
  if (map_width_px <= view_width) {
    camera.x = (view_width - map_width_px) / 2.0F;
  } else {
    camera.x = std::clamp(
        view_width / 2.0F - (static_cast<float>(cell.x) + 0.5F) * cell_size,
        view_width - map_width_px, 0.0F);
  }
  if (map_height_px <= view_height) {
    camera.y = (view_height - map_height_px) / 2.0F;
  } else {
    camera.y = std::clamp(
        view_height / 2.0F - (static_cast<float>(cell.y) + 0.5F) * cell_size,
        view_height - map_height_px, 0.0F);
  }
  return camera;
}

auto InitialFocusCell(const std::vector<Token>& tokens, const FogOfWar& fog,
                      std::string_view player_name) -> std::optional<Cell> {
  const Token* claimable = nullptr;
  const Token* party = nullptr;
  const Token* any = nullptr;
  for (const Token& token : tokens) {
    if (!token.owner.empty() && token.kind == TokenKind::kPlayer &&
        SamePlayerName(token.owner, player_name)) {
      // Своего персонажа игрок должен увидеть даже раньше свободных.
      return token.cell;
    }
    if (claimable == nullptr && token.kind == TokenKind::kPlayer &&
        token.owner.empty()) {
      claimable = &token;
    }
    if (party == nullptr && token.kind == TokenKind::kPlayer) {
      party = &token;
    }
    if (any == nullptr) {
      any = &token;
    }
  }
  if (claimable != nullptr) {
    return claimable->cell;
  }
  if (party != nullptr) {
    return party->cell;
  }
  if (any != nullptr) {
    return any->cell;
  }
  // Токенов в обзоре нет: показываем середину разведанной зоны, чтобы игрок
  // не смотрел в чёрный угол большой карты.
  double sum_x = 0.0;
  double sum_y = 0.0;
  int count = 0;
  for (int y = 0; y < fog.Height(); ++y) {
    for (int x = 0; x < fog.Width(); ++x) {
      if (!fog.IsExplored(Cell{.x = x, .y = y})) {
        continue;
      }
      sum_x += static_cast<double>(x) + 0.5;
      sum_y += static_cast<double>(y) + 0.5;
      ++count;
    }
  }
  if (count == 0) {
    return std::nullopt;
  }
  return Cell{.x = static_cast<int>(sum_x / count),
              .y = static_cast<int>(sum_y / count)};
}

auto CameraCellAt(const Camera& camera, float screen_x, float screen_y)
    -> Cell {
  return Cell{
      .x = static_cast<int>(
          std::floor((screen_x - camera.x) / camera.cell_size)),
      .y = static_cast<int>(
          std::floor((screen_y - camera.y) / camera.cell_size)),
  };
}

auto PanCamera(Camera& camera, float dx, float dy) -> void {
  camera.x += dx;
  camera.y += dy;
}

auto ZoomCamera(Camera& camera, float screen_x, float screen_y, float wheel)
    -> void {
  if (wheel == 0.0F) {
    return;
  }
  const float old_size = camera.cell_size;
  // Точка мира под курсором до масштабирования.
  const float world_x = (screen_x - camera.x) / old_size;
  const float world_y = (screen_y - camera.y) / old_size;
  camera.cell_size =
      std::clamp(old_size + (wheel * kZoomStep), kMinCellSize, kMaxCellSize);
  camera.x = screen_x - (world_x * camera.cell_size);
  camera.y = screen_y - (world_y * camera.cell_size);
}

}  // namespace dnd
