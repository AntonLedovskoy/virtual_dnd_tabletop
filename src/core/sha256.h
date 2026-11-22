#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace dnd {

using Sha256Digest = std::array<std::uint8_t, 32>;

// SHA-256 (FIPS 180-4). Свой компактный код вместо OpenSSL: нужен только
// отпечаток пароля для рукопожатия, а тянуть ради него библиотеку незачем.
// Входные данные целиком помещаются в память; потокового режима нет.
auto Sha256(std::span<const std::uint8_t> data) -> Sha256Digest;
auto Sha256(std::string_view text) -> Sha256Digest;

// Отпечаток в том виде, в каком он лежит в манифесте: 64 символа нижнего
// регистра. Сравнивать хеши строками проще, чем массивами байт.
auto HexDigest(const Sha256Digest& digest) -> std::string;

// HMAC-SHA256 (RFC 2104): отпечаток сообщения с секретным ключом. Именно так
// клиент доказывает знание пароля, не отправляя сам пароль по сети.
auto HmacSha256(std::span<const std::uint8_t> key,
                std::span<const std::uint8_t> message) -> Sha256Digest;

}  // namespace dnd
