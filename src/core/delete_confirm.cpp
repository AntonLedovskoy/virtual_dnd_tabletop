#include "core/delete_confirm.h"

namespace dnd {

DeleteConfirm::DeleteConfirm(Clock::duration delay) : delay_(delay) {}

auto DeleteConfirm::Request(std::size_t id, Clock::time_point now) -> bool {
  if (id_ == id && now <= expires_at_) {
    id_ = kNoConfirmId;
    return true;
  }
  id_ = id;
  expires_at_ = now + delay_;
  return false;
}

}  // namespace dnd
