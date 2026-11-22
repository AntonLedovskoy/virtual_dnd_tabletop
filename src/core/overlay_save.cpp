#include "core/overlay_save.h"

#include <utility>

#include "core/overlay_io.h"

namespace dnd {

OverlaySaver::OverlaySaver() : worker_([this]() -> void { Run(); }) {}

OverlaySaver::~OverlaySaver() {
  {
    const std::scoped_lock lock(mutex_);
    stopping_ = true;
  }
  has_work_.notify_all();
  if (worker_.joinable()) {
    worker_.join();
  }
}

auto OverlaySaver::Request(Overlay overlay, std::filesystem::path path)
    -> void {
  {
    const std::scoped_lock lock(mutex_);
    pending_ = Job{.overlay = std::move(overlay), .path = std::move(path)};
  }
  has_work_.notify_one();
}

auto OverlaySaver::Busy() const -> bool {
  const std::scoped_lock lock(mutex_);
  return running_ || pending_.has_value();
}

auto OverlaySaver::TakeResult() -> std::optional<bool> {
  const std::scoped_lock lock(mutex_);
  const std::optional<bool> result = result_;
  result_.reset();
  return result;
}

auto OverlaySaver::Flush() -> bool {
  std::unique_lock<std::mutex> lock(mutex_);
  idle_.wait(lock,
             [this]() -> bool { return !running_ && !pending_.has_value(); });
  const bool ok = result_.value_or(true);
  result_.reset();
  return ok;
}

auto OverlaySaver::Run() -> void {
  for (;;) {
    Job job;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      has_work_.wait(
          lock, [this]() -> bool { return stopping_ || pending_.has_value(); });
      if (!pending_.has_value()) {
        // Остановка: очередь пуста, незавершённых просьб не осталось.
        return;
      }
      job = std::move(*pending_);
      pending_.reset();
      running_ = true;
    }
    const bool ok = WriteOverlay(job.overlay, job.path);
    {
      const std::scoped_lock lock(mutex_);
      running_ = false;
      result_ = ok;
    }
    idle_.notify_all();
  }
}

}  // namespace dnd
