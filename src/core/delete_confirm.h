#pragma once

#include <chrono>
#include <cstddef>

namespace dnd {

// Повторное Del в этот срок подтверждает удаление токена или сцены.
inline constexpr std::chrono::seconds kDeleteConfirmDelay{3};
// Запроса нет; годится и для индекса сцены 0.
inline constexpr std::size_t kNoConfirmId = static_cast<std::size_t>(-1);

// Защита от случайного удаления: первый Del только предупреждает, второй
// в течение kDeleteConfirmDelay удаляет тот же объект.
class DeleteConfirm {
 public:
  using Clock = std::chrono::steady_clock;

  explicit DeleteConfirm(Clock::duration delay = kDeleteConfirmDelay);

  // true — повтор пришёл вовремя, удаление подтверждено, запрос сброшен.
  auto Request(std::size_t id, Clock::time_point now) -> bool;
  auto Clear() -> void { id_ = kNoConfirmId; }
  [[nodiscard]] auto Pending() const -> std::size_t { return id_; }

 private:
  Clock::duration delay_;
  std::size_t id_ = kNoConfirmId;
  Clock::time_point expires_at_;
};

}  // namespace dnd
