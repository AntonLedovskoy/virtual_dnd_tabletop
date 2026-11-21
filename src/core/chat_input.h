#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace dnd {

// Предел строки чата в байтах.
inline constexpr std::size_t kMaxChatInputBytes = 200;

// Строка чата стола: открытие, набор UTF-8, backspace и отправка. Отделена от
// SDL, чтобы краевые случаи ввода проверялись тестами.
class ChatInput {
 public:
  auto Open() -> void;
  // Открывает с готовым текстом (Enter в режиме шаблона области).
  auto OpenWith(std::string_view prefix) -> void;
  auto Close() -> void;
  auto Append(std::string_view text) -> void;
  auto Backspace() -> void;
  // Возвращает набранное и очищает строку; активность не меняет.
  [[nodiscard]] auto Take() -> std::string;

  [[nodiscard]] auto Active() const -> bool { return active_; }
  [[nodiscard]] auto Text() const -> std::string_view { return text_; }

 private:
  bool active_ = false;
  std::string text_;
};

}  // namespace dnd
