#include "core/map_background.h"

namespace dnd {

auto IsValidMapBackground(const MapBackground& background) -> bool {
  if (background.opacity < kMinBackgroundOpacity ||
      background.opacity > kMaxBackgroundOpacity ||
      background.scale < kMinBackgroundScale ||
      background.scale > kMaxBackgroundScale) {
    return false;
  }
  return background.image.empty() || IsSafeImageName(background.image);
}

}  // namespace dnd
