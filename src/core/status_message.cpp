#include "core/status_message.h"

#include <utility>

namespace dnd {

StatusMessage::StatusMessage(Clock::duration ttl) : ttl_(ttl) {}

auto StatusMessage::Set(std::string text, Clock::time_point now) -> void {
  text_ = std::move(text);
  expires_at_ = now + ttl_;
}

auto StatusMessage::Set(std::string text) -> void {
  Set(std::move(text), Clock::now());
}

auto StatusMessage::Expire(Clock::time_point now) -> void {
  if (!text_.empty() && now >= expires_at_) {
    text_.clear();
  }
}

}  // namespace dnd
