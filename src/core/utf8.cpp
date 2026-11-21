#include "core/utf8.h"

namespace dnd {
namespace {

auto IsContinuationByte(unsigned char byte) -> bool {
  return (byte & 0xC0U) == 0x80U;
}

}  // namespace

auto PreviousCodePointStart(std::string_view text) -> std::size_t {
  if (text.empty()) {
    return 0;
  }
  std::size_t index = text.size() - 1;
  while (index > 0 &&
         IsContinuationByte(static_cast<unsigned char>(text[index]))) {
    --index;
  }
  return index;
}

auto TruncateUtf8(std::string& text, std::size_t max_bytes) -> void {
  if (text.size() <= max_bytes) {
    return;
  }
  std::size_t end = max_bytes;
  while (end > 0 && IsContinuationByte(static_cast<unsigned char>(text[end]))) {
    --end;
  }
  text.resize(end);
}

auto AppendUtf8(std::string& text, std::string_view suffix,
                std::size_t max_bytes) -> void {
  text.append(suffix);
  TruncateUtf8(text, max_bytes);
}

auto EraseLastCodePoint(std::string& text) -> void {
  if (!text.empty()) {
    text.resize(PreviousCodePointStart(text));
  }
}

}  // namespace dnd
