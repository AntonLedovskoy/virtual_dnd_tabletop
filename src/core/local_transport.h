#pragma once

#include <deque>
#include <optional>

#include "core/command.h"
#include "core/event.h"

namespace dnd {

// Внутрипроцессная замена будущего сетевого транспорта. С самого начала
// держит клиент и авторитетный мир за одной границей команд/событий, поэтому
// последующая замена на SDL3_net не затрагивает игровую логику.
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
