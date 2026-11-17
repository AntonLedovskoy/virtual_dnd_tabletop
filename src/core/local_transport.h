#pragma once

#include <deque>
#include <optional>

#include "core/command.h"
#include "core/event.h"

namespace dnd {

// In-process stand-in for the future network transport. It keeps the client
// and the authoritative world behind the same command/event boundary from day
// one, so swapping in SDL3_net later does not touch game logic.
class LocalTransport {
 public:
  auto SubmitCommand(const Command& command) -> void;
  [[nodiscard]] auto NextCommand() -> std::optional<Command>;

  auto PostEvent(const Event& event) -> void;
  [[nodiscard]] auto NextEvent() -> std::optional<Event>;

 private:
  std::deque<Command> commands_;
  std::deque<Event> events_;
};

}  // namespace dnd
