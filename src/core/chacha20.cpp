#include "core/chacha20.h"

#include <algorithm>

namespace dnd {
namespace {

constexpr std::size_t kBlockSize = 64;

auto RotateLeft(std::uint32_t value, int bits) -> std::uint32_t {
  return (value << bits) | (value >> (32 - bits));
}

auto Load32(const std::uint8_t* data) -> std::uint32_t {
  return static_cast<std::uint32_t>(data[0]) |
         (static_cast<std::uint32_t>(data[1]) << 8) |
         (static_cast<std::uint32_t>(data[2]) << 16) |
         (static_cast<std::uint32_t>(data[3]) << 24);
}

auto Store32(std::uint8_t* data, std::uint32_t value) -> void {
  for (int shift = 0; shift < 32; shift += 8) {
    data[shift / 8] = static_cast<std::uint8_t>(value >> shift);
  }
}

auto QuarterRound(std::uint32_t& a, std::uint32_t& b, std::uint32_t& c,
                  std::uint32_t& d) -> void {
  a += b;
  d = RotateLeft(d ^ a, 16);
  c += d;
  b = RotateLeft(b ^ c, 12);
  a += b;
  d = RotateLeft(d ^ a, 8);
  c += d;
  b = RotateLeft(b ^ c, 7);
}

// Один 64-байтный блок ключевого потока: 20 раундов над копией состояния.
auto Block(const std::array<std::uint32_t, 16>& input)
    -> std::array<std::uint32_t, 16> {
  std::array<std::uint32_t, 16> state = input;
  for (int round = 0; round < 10; ++round) {
    QuarterRound(state[0], state[4], state[8], state[12]);
    QuarterRound(state[1], state[5], state[9], state[13]);
    QuarterRound(state[2], state[6], state[10], state[14]);
    QuarterRound(state[3], state[7], state[11], state[15]);
    QuarterRound(state[0], state[5], state[10], state[15]);
    QuarterRound(state[1], state[6], state[11], state[12]);
    QuarterRound(state[2], state[7], state[8], state[13]);
    QuarterRound(state[3], state[4], state[9], state[14]);
  }
  for (std::size_t i = 0; i < state.size(); ++i) {
    state[i] += input[i];
  }
  return state;
}

}  // namespace

auto ChaCha20Xor(const ChaCha20Key& key, const ChaCha20Nonce& nonce,
                 std::uint32_t counter, std::span<const std::uint8_t> input)
    -> std::vector<std::uint8_t> {
  // "expand 32-byte k" из RFC 8439, раздел 2.3.
  std::array<std::uint32_t, 16> state{
      0x61707865U,
      0x3320646EU,
      0x79622D32U,
      0x6B206574U,
  };
  for (std::size_t i = 0; i < 8; ++i) {
    state[4 + i] = Load32(key.data() + (i * 4));
  }
  for (std::size_t i = 0; i < 3; ++i) {
    state[13 + i] = Load32(nonce.data() + (i * 4));
  }

  std::vector<std::uint8_t> output(input.size());
  for (std::size_t offset = 0; offset < input.size(); offset += kBlockSize) {
    state[12] = counter + static_cast<std::uint32_t>(offset / kBlockSize);
    const std::array<std::uint32_t, 16> keystream = Block(state);
    std::array<std::uint8_t, kBlockSize> bytes{};
    for (std::size_t i = 0; i < keystream.size(); ++i) {
      Store32(bytes.data() + (i * 4), keystream[i]);
    }
    const std::size_t chunk = std::min(kBlockSize, input.size() - offset);
    for (std::size_t i = 0; i < chunk; ++i) {
      output[offset + i] = input[offset + i] ^ bytes[i];
    }
  }
  return output;
}

}  // namespace dnd
