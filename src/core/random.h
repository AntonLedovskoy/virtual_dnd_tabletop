#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace dnd {

// Криптостойкие случайные байты. На Windows — системный генератор
// (rand_s над RtlGenRandom), на POSIX — /dev/urandom. std::random_device на
// MinGW вырождается в предсказуемую последовательность, поэтому для ключей и
// запросов рукопожатия он не годится.
// Байты заполняются всегда; false означает, что системный источник отказал и
// пришлось взять std::random_device.
auto FillRandom(std::span<std::uint8_t> bytes) -> bool;

}  // namespace dnd
