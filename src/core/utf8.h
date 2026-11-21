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

// Дописывает текст и обрезает его до max_bytes по границе символа. Так
// набираются поля интерфейса: лимит в байтах не должен резать кириллицу.
auto AppendUtf8(std::string& text, std::string_view suffix,
                std::size_t max_bytes) -> void;

// Удаляет последний UTF-8 символ; пустую строку не трогает.
auto EraseLastCodePoint(std::string& text) -> void;

}  // namespace dnd
