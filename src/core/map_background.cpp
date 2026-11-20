#include "core/map_background.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <string>
#include <string_view>

namespace dnd {
namespace {

constexpr std::array<std::string_view, 4> kAllowedExtensions{".png", ".jpg",
                                                             ".jpeg", ".bmp"};

auto LowerAscii(std::string_view text) -> std::string {
  std::string lowered;
  lowered.reserve(text.size());
  for (const char ch : text) {
    lowered.push_back(
        static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
  }
  return lowered;
}

}  // namespace

auto IsSafeBackgroundImageName(std::string_view name) -> bool {
  if (name.empty() || name.size() > kMaxBackgroundImageName) {
    return false;
  }
  // «..» в любом месте отсекает переходы по каталогам, хотя разделители
  // путей и так запрещены.
  if (name.find("..") != std::string_view::npos) {
    return false;
  }
  const bool has_forbidden = std::ranges::any_of(name, [](char ch) -> bool {
    const auto byte = static_cast<unsigned char>(ch);
    return byte < 0x20 || byte == 0x7F || ch == '/' || ch == '\\' || ch == ':';
  });
  if (has_forbidden) {
    return false;
  }
  const std::size_t dot = name.rfind('.');
  if (dot == std::string_view::npos || dot == 0) {
    return false;
  }
  const std::string extension = LowerAscii(name.substr(dot));
  return std::ranges::any_of(kAllowedExtensions,
                             [&extension](std::string_view allowed) -> bool {
                               return extension == allowed;
                             });
}

auto IsValidMapBackground(const MapBackground& background) -> bool {
  if (background.opacity < kMinBackgroundOpacity ||
      background.opacity > kMaxBackgroundOpacity ||
      background.scale < kMinBackgroundScale ||
      background.scale > kMaxBackgroundScale) {
    return false;
  }
  return background.image.empty() ||
         IsSafeBackgroundImageName(background.image);
}

}  // namespace dnd
