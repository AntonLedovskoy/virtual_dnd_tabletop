#pragma once

#include <chrono>
#include <cstdint>

namespace dnd {

// Секунды с начала эпохи Unix для временных меток лога. 32 бит хватает
// до 2106 года и сохраняет компактный формат передачи.
inline auto CurrentTimeSeconds() -> std::uint32_t {
  return static_cast<std::uint32_t>(
      std::chrono::duration_cast<std::chrono::seconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
}

}  // namespace dnd
