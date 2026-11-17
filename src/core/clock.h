#pragma once

#include <chrono>
#include <cstdint>

namespace dnd {

// Seconds since the Unix epoch, used for log timestamps. 32 bits are enough
// until 2106 and keep the wire format compact.
inline auto CurrentTimeSeconds() -> std::uint32_t {
  return static_cast<std::uint32_t>(
      std::chrono::duration_cast<std::chrono::seconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
}

}  // namespace dnd
