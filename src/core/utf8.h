#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace dnd {

// Возвращает смещение начала последнего UTF-8 символа. Используется для
// backspace и обрезки, чтобы многобайтовые символы кириллицы не ломались.
[[nodiscard]] auto PreviousCodePointStart(std::string_view text) -> std::size_t;

// Обрезает по границе символа, никогда внутри UTF-8 последовательности.
auto TruncateUtf8(std::string& text, std::size_t max_bytes) -> void;

}  // namespace dnd
