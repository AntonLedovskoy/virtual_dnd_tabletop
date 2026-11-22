#pragma once

#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <optional>
#include <thread>

#include "core/overlay.h"

namespace dnd {

// Запись партии в отдельном потоке: кадр не ждёт диск, а оверлей растёт с
// каждым сыгранным ходом. Копия снимается в момент Request, поэтому играть
// можно сразу.
class OverlaySaver {
 public:
  OverlaySaver();
  ~OverlaySaver();
  OverlaySaver(const OverlaySaver&) = delete;
  auto operator=(const OverlaySaver&) -> OverlaySaver& = delete;

  // Новая просьба заменяет ещё не начатую: на диск уйдёт самый свежий оверлей.
  // Уже идущая запись дописывается, следом идёт эта.
  auto Request(Overlay overlay, std::filesystem::path path) -> void;
  // Есть незаписанная партия или идёт запись.
  [[nodiscard]] auto Busy() const -> bool;
  // Результат записи, завершившейся с прошлого опроса; nullopt — нечего.
  auto TakeResult() -> std::optional<bool>;
  // Дождаться диска (выход, явное сохранение): true — последняя запись удалась.
  auto Flush() -> bool;

 private:
  struct Job {
    Overlay overlay;
    std::filesystem::path path;
  };

  auto Run() -> void;

  mutable std::mutex mutex_;
  std::condition_variable has_work_;
  std::condition_variable idle_;
  std::optional<Job> pending_;
  std::optional<bool> result_;
  bool running_ = false;
  bool stopping_ = false;
  std::thread worker_;
};

}  // namespace dnd
