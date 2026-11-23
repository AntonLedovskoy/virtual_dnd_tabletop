#pragma once

#include <optional>
#include <string_view>
#include <vector>

#include "core/grid_map.h"
#include "core/token.h"
#include "core/visibility.h"

namespace dnd {

inline constexpr float kDefaultCellSize = 48.0F;
inline constexpr float kMinCellSize = 24.0F;
inline constexpr float kMaxCellSize = 96.0F;
// Насколько меняется клетка за одно деление колеса.
inline constexpr float kZoomStep = 4.0F;

// Положение карты на экране: размер клетки и смещение левого верхнего угла.
// Держится отдельно от SDL, чтобы прокрутку и масштаб можно было проверить
// тестами без окна.
struct Camera {
  float cell_size = kDefaultCellSize;
  float x = 0.0F;
  float y = 0.0F;
};

// Камера, в которой карта целиком стоит по центру окна.
[[nodiscard]] auto CenteredCamera(float view_width, float view_height,
                                  int map_width, int map_height,
                                  float cell_size = kDefaultCellSize) -> Camera;

// Камера, в которой клетка стоит по центру окна. Карта не уезжает за край:
// если она меньше окна, вид по-прежнему центрируется целиком.
[[nodiscard]] auto CameraOnCell(float view_width, float view_height,
                                int map_width, int map_height, Cell cell,
                                float cell_size = kDefaultCellSize) -> Camera;

// Куда навести камеру игроку сразу после входа: свой персонаж, иначе
// свободный персонаж для выбора, иначе персонаж партии, иначе разведанная
// зона карты. nullopt — показывать карту целиком.
[[nodiscard]] auto InitialFocusCell(const std::vector<Token>& tokens,
                                    const FogOfWar& fog,
                                    std::string_view player_name)
    -> std::optional<Cell>;

// Клетка под точкой экрана; за краем карты координаты отрицательные или
// больше размера.
[[nodiscard]] auto CameraCellAt(const Camera& camera, float screen_x,
                                float screen_y) -> Cell;

// Сдвигает вид вслед за перетаскиванием средней кнопкой.
auto PanCamera(Camera& camera, float dx, float dy) -> void;

// Приближает или отдаляет карту под курсором: точка мира под мышью остаётся
// на месте. Колесо — в делениях, размер клетки ограничен
// kMinCellSize..kMaxCellSize.
auto ZoomCamera(Camera& camera, float screen_x, float screen_y, float wheel)
    -> void;

}  // namespace dnd
