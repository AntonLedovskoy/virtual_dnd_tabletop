#pragma once

#include <cstdint>
#include <string>

namespace dnd {

// One line of the shared table log: chat, dice rolls and system messages.
// timestamp is Unix seconds; 0 means "unknown" and is rendered without a time.
struct LogLine {
  std::string author;
  std::string text;
  std::uint32_t timestamp = 0;
};

}  // namespace dnd
