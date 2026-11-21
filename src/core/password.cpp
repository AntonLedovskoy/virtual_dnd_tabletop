#include "core/password.h"

#include "core/random.h"

namespace dnd {

auto RandomChallenge() -> ChallengeNonce {
  ChallengeNonce nonce;
  // FillRandom заполняет байты в любом случае: отказ системного источника
  // уже обработан внутри.
  (void)FillRandom(nonce);
  return nonce;
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
