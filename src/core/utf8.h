#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace dnd {

// Returns the byte offset where the last UTF-8 code point starts. Used for
// backspace and text truncation so multi-byte Cyrillic characters survive.
[[nodiscard]] auto PreviousCodePointStart(std::string_view text) -> std::size_t;

// Truncates at a code point boundary, never inside a UTF-8 sequence.
auto TruncateUtf8(std::string& text, std::size_t max_bytes) -> void;

}  // namespace dnd
