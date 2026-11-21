#include "core/random.h"

#include <algorithm>
#include <random>

#ifdef _WIN32
#include <cstdlib>
#else
#include <fstream>
#endif

namespace dnd {
namespace {

// Крайний случай: системный источник недоступен. std::random_device на
// MinGW предсказуем, поэтому это хуже, чем rand_s, но лучше нулевого ключа.
auto FillFromRandomDevice(std::span<std::uint8_t> bytes) -> void {
  std::random_device device;
  for (std::uint8_t& byte : bytes) {
    byte = static_cast<std::uint8_t>(device());
  }
}

}  // namespace

auto FillRandom(std::span<std::uint8_t> bytes) -> bool {
  if (bytes.empty()) {
    return true;
  }
#ifdef _WIN32
  // rand_s — обёртка над системным RtlGenRandom: по 4 байта за вызов.
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    unsigned int value = 0;
    if (rand_s(&value) != 0) {
      FillFromRandomDevice(bytes.subspan(offset));
      return false;
    }
    for (std::size_t i = 0; i < 4 && offset < bytes.size(); ++i) {
      bytes[offset] = static_cast<std::uint8_t>(value >> (i * 8));
      ++offset;
    }
  }
  return true;
#else
  std::ifstream source("/dev/urandom", std::ios::binary);
  if (!source) {
    FillFromRandomDevice(bytes);
    return false;
  }
  source.read(reinterpret_cast<char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
  if (source.gcount() != static_cast<std::streamsize>(bytes.size())) {
    FillFromRandomDevice(bytes);
    return false;
  }
  return true;
#endif
}

}  // namespace dnd
