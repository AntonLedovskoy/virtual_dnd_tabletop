#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include "core/chacha20.h"
#include "core/password.h"
#include "core/sha256.h"
#include "core/x25519.h"

namespace dnd {

// Маркер шифрованного кадра. Обычный кадр начинается с типа сообщения
// (1..9), поэтому маркер вне этого диапазона с ними не путается.
inline constexpr std::uint8_t kSealedFrameMarker = 0xF2;
// Маркер, номер кадра, и MAC: столько байт шифрованный кадр добавляет.
inline constexpr std::size_t kSealedFrameOverhead = 1 + 8 + kProofSize;

// Ключи сессии: своя пара (шифр, MAC) на каждое направление данных.
// Направления разные, чтобы одинаковые кадры не давали одинаковый поток.
struct SessionSecrets {
  ChaCha20Key client_to_server_key{};
  ChaCha20Key server_to_client_key{};
  Sha256Digest client_to_server_mac{};
  Sha256Digest server_to_client_mac{};
};

// HKDF-SHA256 (RFC 5869): из общего секрета делает ключи нужной длины.
auto HkdfSha256(std::span<const std::uint8_t> ikm,
                std::span<const std::uint8_t> salt,
                std::span<const std::uint8_t> info, std::size_t length)
    -> std::vector<std::uint8_t>;

// Транскрипт рукопожатия: запрос хоста, оба открытых ключа и имя игрока.
// Отпечатки пароля подписывают именно его, поэтому подмена ключей посреди
// соединения не проходит.
auto HandshakeTranscript(const ChallengeNonce& nonce,
                         const X25519Key& host_public,
                         const X25519Key& client_public,
                         std::string_view player_name)
    -> std::vector<std::uint8_t>;

// Клиент доказывает знание пароля, хост — тоже: иначе клиент не отличит
// настоящий хост от перехватчика, который ключи подменил.
auto MakeClientProof(std::string_view password,
                     std::span<const std::uint8_t> transcript) -> PasswordProof;
auto MakeServerProof(std::string_view password,
                     std::span<const std::uint8_t> transcript) -> PasswordProof;

// Ключи из общего секрета X25519; соль — запрос хоста, поэтому у каждой
// сессии они свои.
auto DeriveSessionSecrets(const X25519Key& shared, const ChallengeNonce& nonce)
    -> SessionSecrets;

// Шифрование кадров: ChaCha20 и HMAC-SHA256 по схеме encrypt-then-MAC.
// Номер кадра входит в MAC: переставить, повторить или обрезать кадр нельзя.
class SessionCipher {
 public:
  SessionCipher() = default;

  static auto ForHost(const SessionSecrets& secrets) -> SessionCipher;
  static auto ForClient(const SessionSecrets& secrets) -> SessionCipher;

  // [маркер][номер u64][шифртекст][MAC 32].
  auto Seal(std::span<const std::uint8_t> payload) -> std::vector<std::uint8_t>;
  // Проверяет маркер, номер и MAC; при любой ошибке payload не трогает.
  auto Open(std::span<const std::uint8_t> frame,
            std::vector<std::uint8_t>& payload) -> bool;

 private:
  ChaCha20Key send_key_{};
  ChaCha20Key receive_key_{};
  Sha256Digest send_mac_{};
  Sha256Digest receive_mac_{};
  std::uint64_t send_sequence_ = 0;
  std::uint64_t receive_sequence_ = 0;
};

}  // namespace dnd
