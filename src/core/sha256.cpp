#include "core/sha256.h"

#include <algorithm>
#include <vector>

namespace dnd {
namespace {

constexpr std::size_t kBlockSize = 64;
constexpr std::size_t kDigestSize = 32;
// Раундовые константы SHA-256 (FIPS 180-4, раздел 4.2.2).
constexpr std::array<std::uint32_t, 64> kRoundConstants{
    0x428A2F98U, 0x71374491U, 0xB5C0FBCFU, 0xE9B5DBA5U, 0x3956C25BU,
    0x59F111F1U, 0x923F82A4U, 0xAB1C5ED5U, 0xD807AA98U, 0x12835B01U,
    0x243185BEU, 0x550C7DC3U, 0x72BE5D74U, 0x80DEB1FEU, 0x9BDC06A7U,
    0xC19BF174U, 0xE49B69C1U, 0xEFBE4786U, 0x0FC19DC6U, 0x240CA1CCU,
    0x2DE92C6FU, 0x4A7484AAU, 0x5CB0A9DCU, 0x76F988DAU, 0x983E5152U,
    0xA831C66DU, 0xB00327C8U, 0xBF597FC7U, 0xC6E00BF3U, 0xD5A79147U,
    0x06CA6351U, 0x14292967U, 0x27B70A85U, 0x2E1B2138U, 0x4D2C6DFCU,
    0x53380D13U, 0x650A7354U, 0x766A0ABBU, 0x81C2C92EU, 0x92722C85U,
    0xA2BFE8A1U, 0xA81A664BU, 0xC24B8B70U, 0xC76C51A3U, 0xD192E819U,
    0xD6990624U, 0xF40E3585U, 0x106AA070U, 0x19A4C116U, 0x1E376C08U,
    0x2748774CU, 0x34B0BCB5U, 0x391C0CB3U, 0x4ED8AA4AU, 0x5B9CCA4FU,
    0x682E6FF3U, 0x748F82EEU, 0x78A5636FU, 0x84C87814U, 0x8CC70208U,
    0x90BEFFFAU, 0xA4506CEBU, 0xBEF9A3F7U, 0xC67178F2U,
};

auto RotateRight(std::uint32_t value, int bits) -> std::uint32_t {
  return (value >> bits) | (value << (32 - bits));
}

auto ReadBigEndian32(const std::uint8_t* data) -> std::uint32_t {
  return (static_cast<std::uint32_t>(data[0]) << 24) |
         (static_cast<std::uint32_t>(data[1]) << 16) |
         (static_cast<std::uint32_t>(data[2]) << 8) |
         static_cast<std::uint32_t>(data[3]);
}

// Один блок в 64 байта: 64 раунда над восемью словами состояния.
auto Compress(std::array<std::uint32_t, 8>& state, const std::uint8_t* block)
    -> void {
  std::array<std::uint32_t, 64> schedule{};
  for (std::size_t i = 0; i < 16; ++i) {
    schedule[i] = ReadBigEndian32(block + (i * 4));
  }
  for (std::size_t i = 16; i < 64; ++i) {
    const std::uint32_t s0 = RotateRight(schedule[i - 15], 7) ^
                             RotateRight(schedule[i - 15], 18) ^
                             (schedule[i - 15] >> 3);
    const std::uint32_t s1 = RotateRight(schedule[i - 2], 17) ^
                             RotateRight(schedule[i - 2], 19) ^
                             (schedule[i - 2] >> 10);
    schedule[i] = schedule[i - 16] + s0 + schedule[i - 7] + s1;
  }

  std::uint32_t a = state[0];
  std::uint32_t b = state[1];
  std::uint32_t c = state[2];
  std::uint32_t d = state[3];
  std::uint32_t e = state[4];
  std::uint32_t f = state[5];
  std::uint32_t g = state[6];
  std::uint32_t h = state[7];
  for (std::size_t i = 0; i < 64; ++i) {
    const std::uint32_t sum1 =
        RotateRight(e, 6) ^ RotateRight(e, 11) ^ RotateRight(e, 25);
    const std::uint32_t choose = (e & f) ^ (~e & g);
    const std::uint32_t temp1 =
        h + sum1 + choose + kRoundConstants[i] + schedule[i];
    const std::uint32_t sum0 =
        RotateRight(a, 2) ^ RotateRight(a, 13) ^ RotateRight(a, 22);
    const std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
    const std::uint32_t temp2 = sum0 + majority;
    h = g;
    g = f;
    f = e;
    e = d + temp1;
    d = c;
    c = b;
    b = a;
    a = temp1 + temp2;
  }
  state[0] += a;
  state[1] += b;
  state[2] += c;
  state[3] += d;
  state[4] += e;
  state[5] += f;
  state[6] += g;
  state[7] += h;
}

}  // namespace

auto Sha256(std::span<const std::uint8_t> data) -> Sha256Digest {
  std::array<std::uint32_t, 8> state{
      0x6A09E667U, 0xBB67AE85U, 0x3C6EF372U, 0xA54FF53AU,
      0x510E527FU, 0x9B05688CU, 0x1F83D9ABU, 0x5BE0CD19U,
  };

  // Дополнение по FIPS 180-4: бит 1, нули до 56 байт остатка и длина в битах.
  std::vector<std::uint8_t> padded(data.begin(), data.end());
  const auto bit_length = static_cast<std::uint64_t>(data.size()) * 8;
  padded.push_back(0x80);
  while (padded.size() % kBlockSize != 56) {
    padded.push_back(0);
  }
  for (int shift = 56; shift >= 0; shift -= 8) {
    padded.push_back(static_cast<std::uint8_t>((bit_length >> shift) & 0xFFU));
  }

  for (std::size_t offset = 0; offset < padded.size(); offset += kBlockSize) {
    Compress(state, padded.data() + offset);
  }

  Sha256Digest digest{};
  for (std::size_t i = 0; i < state.size(); ++i) {
    digest[(i * 4) + 0] = static_cast<std::uint8_t>(state[i] >> 24);
    digest[(i * 4) + 1] = static_cast<std::uint8_t>(state[i] >> 16);
    digest[(i * 4) + 2] = static_cast<std::uint8_t>(state[i] >> 8);
    digest[(i * 4) + 3] = static_cast<std::uint8_t>(state[i]);
  }
  return digest;
}

auto Sha256(std::string_view text) -> Sha256Digest {
  const auto* bytes = reinterpret_cast<const std::uint8_t*>(text.data());
  return Sha256(std::span<const std::uint8_t>(bytes, text.size()));
}

auto HexDigest(const Sha256Digest& digest) -> std::string {
  constexpr std::array<char, 16> hex_digits = {'0', '1', '2', '3', '4', '5',
                                               '6', '7', '8', '9', 'a', 'b',
                                               'c', 'd', 'e', 'f'};
  std::string text;
  text.reserve(digest.size() * 2);
  for (const std::uint8_t byte : digest) {
    text.push_back(hex_digits[byte >> 4]);
    text.push_back(hex_digits[byte & 0x0F]);
  }
  return text;
}

auto HmacSha256(std::span<const std::uint8_t> key,
                std::span<const std::uint8_t> message) -> Sha256Digest {
  std::array<std::uint8_t, kBlockSize> block{};
  if (key.size() > block.size()) {
    const Sha256Digest reduced = Sha256(key);
    std::ranges::copy(reduced, block.begin());
  } else {
    std::ranges::copy(key, block.begin());
  }

  std::vector<std::uint8_t> inner;
  inner.reserve(kBlockSize + message.size());
  for (const std::uint8_t byte : block) {
    inner.push_back(byte ^ 0x36U);
  }
  inner.insert(inner.end(), message.begin(), message.end());
  const Sha256Digest inner_digest = Sha256(inner);

  std::vector<std::uint8_t> outer;
  outer.reserve(kBlockSize + kDigestSize);
  for (const std::uint8_t byte : block) {
    outer.push_back(byte ^ 0x5CU);
  }
  outer.insert(outer.end(), inner_digest.begin(), inner_digest.end());
  return Sha256(outer);
}

}  // namespace dnd
