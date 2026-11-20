#pragma once

#include <cstddef>
#include <string_view>

namespace dnd {

inline constexpr std::size_t kMaxImageNameLength = 128;

// Имя картинки пригодно для обмена по сети: без каталогов и переходов вверх
// по дереву, с расширением, которое умеет читать SDL3_image. Так сцена и
// токены хранят только имя файла рядом с .exe, а не произвольный путь.
[[nodiscard]] auto IsSafeImageName(std::string_view name) -> bool;

}  // namespace dnd
