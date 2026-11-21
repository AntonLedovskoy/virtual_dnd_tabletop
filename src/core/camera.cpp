#include "core/camera.h"

#include <algorithm>
#include <cmath>

namespace dnd {

auto CenteredCamera(float view_width, float view_height, int map_width,
                    int map_height, float cell_size) -> Camera {
  Camera camera{.cell_size = cell_size};
  camera.x = (view_width - static_cast<float>(map_width) * cell_size) / 2.0F;
  camera.y = (view_height - static_cast<float>(map_height) * cell_size) / 2.0F;
  return camera;
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
