#pragma once

#include <chrono>
#include <string>
#include <string_view>

namespace dnd {

// Сколько висит сообщение в строке статуса модальной панели.
inline constexpr std::chrono::seconds kStatusMessageTtl{3};

// Короткое сообщение панели: живёт kStatusMessageTtl, потом строка статуса
// снова уступает место подсказке.
class StatusMessage {
 public:
  using Clock = std::chrono::steady_clock;

  explicit StatusMessage(Clock::duration ttl = kStatusMessageTtl);

  // Новое сообщение заменяет старое и заново запускает срок.
  auto Set(std::string text, Clock::time_point now) -> void;
  auto Set(std::string text) -> void;
  auto Clear() -> void { text_.clear(); }
  // Гасит сообщение с истёкшим сроком; вызывается каждый кадр.
  auto Expire(Clock::time_point now) -> void;

  [[nodiscard]] auto Text() const -> const std::string& { return text_; }
  [[nodiscard]] auto Empty() const -> bool { return text_.empty(); }

 private:
  Clock::duration ttl_;
  Clock::time_point expires_at_;
  std::string text_;
};

}  // namespace dnd
