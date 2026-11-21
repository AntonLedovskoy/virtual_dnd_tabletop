#include "core/navigation.h"

#include <cstdint>

namespace dnd {
namespace {

auto ClampIndex(std::size_t current, std::size_t count) -> std::size_t {
  if (count == 0) {
    return 0;
  }
  return current < count ? current : count - 1;
}

}  // namespace

auto WrapIndex(std::size_t current, int delta, std::size_t count)
    -> std::size_t {
  if (count == 0) {
    return 0;
  }
  const auto size = static_cast<std::int64_t>(count);
  std::int64_t next = (static_cast<std::int64_t>(current) + delta) % size;
  if (next < 0) {
    next += size;
  }
  return static_cast<std::size_t>(next);
}

auto StepIndex(std::size_t current, int delta, std::size_t count)
    -> std::size_t {
  if (count == 0) {
    return 0;
  }
  const std::size_t index = ClampIndex(current, count);
  if (delta < 0) {
    return index > 0 ? index - 1 : 0;
  }
  if (delta > 0) {
    return index + 1 < count ? index + 1 : count - 1;
  }
  return index;
}

auto IndexAfterDelete(std::size_t removed, std::size_t remaining)
    -> std::size_t {
  if (remaining == 0) {
    return 0;
  }
  const std::size_t previous = removed == 0 ? 0 : removed - 1;
  return previous < remaining ? previous : remaining - 1;
}

auto IndexForDigit(int digit, std::size_t count) -> std::optional<std::size_t> {
  if (digit < 1 || digit > 9) {
    return std::nullopt;
  }
  const auto index = static_cast<std::size_t>(digit - 1);
  if (index >= count) {
    return std::nullopt;
  }
  return index;
}

}  // namespace dnd
