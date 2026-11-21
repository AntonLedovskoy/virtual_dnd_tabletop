#include "core/autosave.h"

namespace dnd {

AutosaveTimer::AutosaveTimer(Clock::duration delay) : delay_(delay) {}

auto AutosaveTimer::MarkChanged(Clock::time_point now) -> void {
  if (!dirty_) {
    dirty_ = true;
    changed_at_ = now;
  }
}

auto AutosaveTimer::Due(Clock::time_point now) const -> bool {
  return dirty_ && now - changed_at_ >= delay_;
}

auto AutosaveTimer::Saved() -> void { dirty_ = false; }

auto AutosaveTimer::SaveFailed(Clock::time_point now) -> void {
  if (dirty_) {
    changed_at_ = now;
  }
}

auto AutosaveTimer::ChangedDuringSave(Clock::time_point now) -> void {
  dirty_ = true;
  changed_at_ = now;
}

}  // namespace dnd
