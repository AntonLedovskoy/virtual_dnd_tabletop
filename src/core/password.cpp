#include "core/password.h"

#include <random>
#include <span>

#include "core/sha256.h"

namespace dnd {

auto RandomChallenge() -> ChallengeNonce {
  ChallengeNonce nonce;
  std::random_device device;
  for (std::uint8_t& byte : nonce) {
    byte = static_cast<std::uint8_t>(device());
  }
  return nonce;
}

auto MakePasswordProof(std::string_view password, const ChallengeNonce& nonce)
    -> PasswordProof {
  const auto* key = reinterpret_cast<const std::uint8_t*>(password.data());
  return HmacSha256(std::span<const std::uint8_t>(key, password.size()),
                    std::span<const std::uint8_t>(nonce));
}

auto ProofMatches(const PasswordProof& expected, const PasswordProof& actual)
    -> bool {
  std::uint8_t difference = 0;
  for (std::size_t i = 0; i < kProofSize; ++i) {
    difference |= static_cast<std::uint8_t>(expected[i] ^ actual[i]);
  }
  return difference == 0;
}

}  // namespace dnd
