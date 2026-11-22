#include "core/map_background.h"

#include "core/chat_command.h"

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

auto ApplyMapCommand(MapBackground& background, const ChatCommand& command)
    -> bool {
  if (command.error != ChatCommandError::kNone) {
    return false;
  }
  switch (command.kind) {
    case ChatCommandKind::kMapOff:
      background = MapBackground{};
      return true;
    case ChatCommandKind::kMapOpacity:
      background.opacity = command.value;
      return true;
    case ChatCommandKind::kMapScale:
      background.scale = command.value;
      return true;
    case ChatCommandKind::kMapImage:
      background.image = command.text;
      return true;
    default:
      return false;
  }
}

}  // namespace dnd
