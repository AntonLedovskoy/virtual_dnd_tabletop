#pragma once

#include <chrono>

namespace dnd {

// Пауза после первого изменения мира: серия ходов не должна писать файл
// кампании каждый кадр.
inline constexpr std::chrono::seconds kAutosaveDelay{8};

// Отложенная запись кампании. Отсчёт идёт от первого несохранённого
// изменения, поэтому непрерывные ходы не отодвигают автосейв до бесконечности.
class AutosaveTimer {
 public:
  using Clock = std::chrono::steady_clock;

  explicit AutosaveTimer(Clock::duration delay = kAutosaveDelay);

  // Мир изменился; повторные вызовы не сдвигают момент записи.
  auto MarkChanged(Clock::time_point now) -> void;
  [[nodiscard]] auto Dirty() const -> bool { return dirty_; }
  // Пора ли писать на диск.
  [[nodiscard]] auto Due(Clock::time_point now) const -> bool;
  // Запись удалась: ждать больше нечего.
  auto Saved() -> void;
  // Запись не удалась: повтор через полную паузу, а не каждый кадр.
  auto SaveFailed(Clock::time_point now) -> void;

 private:
  Clock::duration delay_;
  bool dirty_ = false;
  Clock::time_point changed_at_;
};

}  // namespace dnd
