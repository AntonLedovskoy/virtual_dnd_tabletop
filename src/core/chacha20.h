#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace dnd {

// ChaCha20 (RFC 8439): потоковый шифр для кадров сессии. Свой код, как и
// с SHA-256: нужен только XOR с потоком ключа, зависимость не оправдана.
inline constexpr std::size_t kChaCha20KeySize = 32;
inline constexpr std::size_t kChaCha20NonceSize = 12;

using ChaCha20Key = std::array<std::uint8_t, kChaCha20KeySize>;
using ChaCha20Nonce = std::array<std::uint8_t, kChaCha20NonceSize>;

// XOR данных с ключевым потоком, начиная с блока counter. Шифрование и
// расшифровка — одна и та же операция.
auto ChaCha20Xor(const ChaCha20Key& key, const ChaCha20Nonce& nonce,
                 std::uint32_t counter, std::span<const std::uint8_t> input)
    -> std::vector<std::uint8_t>;

}  // namespace dnd
