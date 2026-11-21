#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace dnd {

// Ключ X25519: обмен ключами на эллиптической кривой Curve25519 (RFC 7748).
// Свой код, как и с SHA-256: нужен один обмен на соединение, а тянуть
// криптобиблиотеку ради него незачем.
inline constexpr std::size_t kX25519KeySize = 32;

using X25519Key = std::array<std::uint8_t, kX25519KeySize>;

struct X25519KeyPair {
  X25519Key private_key{};
  X25519Key public_key{};
};

// Новая пара на каждое соединение: её знает только эта сессия, поэтому
// записанный трафик не расшифровать и после утечки пароля.
auto GenerateX25519KeyPair() -> X25519KeyPair;

// Открытый ключ из закрытого: скаляр, умноженный на базовую точку.
auto X25519PublicKey(const X25519Key& private_key) -> X25519Key;

// Общий секрет из своего закрытого и чужого открытого ключа. false, если
// собеседник прислал точку малого порядка: такой секрет всегда нулевой.
auto X25519SharedSecret(const X25519Key& private_key,
                        const X25519Key& peer_public, X25519Key& shared)
    -> bool;

}  // namespace dnd
