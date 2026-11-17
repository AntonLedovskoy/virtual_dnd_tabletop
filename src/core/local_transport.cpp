#include "core/local_transport.h"

namespace dnd {

auto LocalTransport::SubmitCommand(const Command& command) -> void {
  commands_.push_back(command);
}

auto LocalTransport::NextCommand() -> std::optional<Command> {
  if (commands_.empty()) {
    return std::nullopt;
  }
  Command command = commands_.front();
  commands_.pop_front();
  return command;
}

auto LocalTransport::PostEvent(const Event& event) -> void {
  events_.push_back(event);
}

auto LocalTransport::NextEvent() -> std::optional<Event> {
  if (events_.empty()) {
    return std::nullopt;
  }
  Event event = events_.front();
  events_.pop_front();
  return event;
}

}  // namespace dnd
