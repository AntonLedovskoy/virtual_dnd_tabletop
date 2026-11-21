#include "core/chat_input.h"

#include <utility>

#include "core/utf8.h"

namespace dnd {

auto ChatInput::Open() -> void {
  active_ = true;
  text_.clear();
}

auto ChatInput::OpenWith(std::string_view prefix) -> void {
  active_ = true;
  text_ = std::string(prefix);
  TruncateUtf8(text_, kMaxChatInputBytes);
}

auto ChatInput::Close() -> void {
  active_ = false;
  text_.clear();
}

auto ChatInput::Append(std::string_view text) -> void {
  AppendUtf8(text_, text, kMaxChatInputBytes);
}

auto ChatInput::Backspace() -> void { EraseLastCodePoint(text_); }

auto ChatInput::Take() -> std::string {
  std::string text = std::move(text_);
  text_.clear();
  return text;
}

}  // namespace dnd
