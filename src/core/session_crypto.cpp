#include "core/session_crypto.h"

#include <algorithm>

namespace dnd {
namespace {

constexpr std::size_t kHashSize = 32;
constexpr std::string_view kClientLabel = "dnd/1 client proof";
constexpr std::string_view kServerLabel = "dnd/1 server proof";
constexpr std::string_view kSessionInfo = "dnd/1 session keys";

auto AsBytes(std::string_view text) -> std::span<const std::uint8_t> {
  const auto* data = reinterpret_cast<const std::uint8_t*>(text.data());
  return {data, text.size()};
}

auto AsBytes(const Sha256Digest& digest) -> std::span<const std::uint8_t> {
  return {digest};
}

auto MakeProof(std::string_view password, std::string_view label,
               std::span<const std::uint8_t> transcript) -> PasswordProof {
  std::vector<std::uint8_t> message;
  message.reserve(label.size() + transcript.size());
  const std::span<const std::uint8_t> label_bytes = AsBytes(label);
  message.insert(message.end(), label_bytes.begin(), label_bytes.end());
  message.insert(message.end(), transcript.begin(), transcript.end());
  return HmacSha256(AsBytes(password), message);
}

// ChaCha20 не повторяет поток: номер кадра — вторая половина nonce.
auto SequenceNonce(std::uint64_t sequence) -> ChaCha20Nonce {
  ChaCha20Nonce nonce{};
  for (std::size_t i = 0; i < 8; ++i) {
    nonce[4 + i] = static_cast<std::uint8_t>(sequence >> (i * 8));
  }
  return nonce;
}

// Сравнение MAC без раннего выхода: время не выдаёт, какой байт разошёлся.
auto TagMatches(std::span<const std::uint8_t> expected,
                std::span<const std::uint8_t> actual) -> bool {
  if (expected.size() != actual.size()) {
    return false;
  }
  std::uint8_t difference = 0;
  for (std::size_t i = 0; i < expected.size(); ++i) {
    difference |= static_cast<std::uint8_t>(expected[i] ^ actual[i]);
  }
  return difference == 0;
}

}  // namespace

auto HkdfSha256(std::span<const std::uint8_t> ikm,
                std::span<const std::uint8_t> salt,
                std::span<const std::uint8_t> info, std::size_t length)
    -> std::vector<std::uint8_t> {
  const Sha256Digest prk = HmacSha256(salt, ikm);

  std::vector<std::uint8_t> output;
  output.reserve(std::min(length, kHashSize * 255));
  std::vector<std::uint8_t> previous;
  for (std::uint8_t counter = 1; output.size() < length; ++counter) {
    std::vector<std::uint8_t> message = previous;
    message.insert(message.end(), info.begin(), info.end());
    message.push_back(counter);
    const Sha256Digest digest = HmacSha256(AsBytes(prk), message);
    previous.assign(digest.begin(), digest.end());
    const std::size_t take = std::min(kHashSize, length - output.size());
    output.insert(output.end(), previous.begin(),
                  previous.begin() + static_cast<std::ptrdiff_t>(take));
  }
  return output;
}

auto HandshakeTranscript(const ChallengeNonce& nonce,
                         const X25519Key& host_public,
                         const X25519Key& client_public,
                         std::string_view player_name)
    -> std::vector<std::uint8_t> {
  std::vector<std::uint8_t> transcript;
  transcript.reserve(nonce.size() + host_public.size() + client_public.size() +
                     player_name.size());
  transcript.insert(transcript.end(), nonce.begin(), nonce.end());
  transcript.insert(transcript.end(), host_public.begin(), host_public.end());
  transcript.insert(transcript.end(), client_public.begin(),
                    client_public.end());
  const std::span<const std::uint8_t> name = AsBytes(player_name);
  transcript.insert(transcript.end(), name.begin(), name.end());
  return transcript;
}

auto MakeClientProof(std::string_view password,
                     std::span<const std::uint8_t> transcript)
    -> PasswordProof {
  return MakeProof(password, kClientLabel, transcript);
}

auto MakeServerProof(std::string_view password,
                     std::span<const std::uint8_t> transcript)
    -> PasswordProof {
  return MakeProof(password, kServerLabel, transcript);
}

auto DeriveSessionSecrets(const X25519Key& shared, const ChallengeNonce& nonce)
    -> SessionSecrets {
  const std::vector<std::uint8_t> okm =
      HkdfSha256(shared, nonce, AsBytes(kSessionInfo), kHashSize * 4);
  SessionSecrets secrets;
  const auto copy_key = [&okm](std::size_t offset, ChaCha20Key& key) -> void {
    std::copy_n(okm.begin() + static_cast<std::ptrdiff_t>(offset), key.size(),
                key.begin());
  };
  copy_key(0, secrets.client_to_server_key);
  copy_key(kHashSize, secrets.server_to_client_key);
  std::copy_n(okm.begin() + static_cast<std::ptrdiff_t>(kHashSize * 2),
              kHashSize, secrets.client_to_server_mac.begin());
  std::copy_n(okm.begin() + static_cast<std::ptrdiff_t>(kHashSize * 3),
              kHashSize, secrets.server_to_client_mac.begin());
  return secrets;
}

auto SessionCipher::ForHost(const SessionSecrets& secrets) -> SessionCipher {
  SessionCipher cipher;
  cipher.send_key_ = secrets.server_to_client_key;
  cipher.receive_key_ = secrets.client_to_server_key;
  cipher.send_mac_ = secrets.server_to_client_mac;
  cipher.receive_mac_ = secrets.client_to_server_mac;
  return cipher;
}

auto SessionCipher::ForClient(const SessionSecrets& secrets) -> SessionCipher {
  SessionCipher cipher;
  cipher.send_key_ = secrets.client_to_server_key;
  cipher.receive_key_ = secrets.server_to_client_key;
  cipher.send_mac_ = secrets.client_to_server_mac;
  cipher.receive_mac_ = secrets.server_to_client_mac;
  return cipher;
}

auto SessionCipher::Seal(std::span<const std::uint8_t> payload)
    -> std::vector<std::uint8_t> {
  std::vector<std::uint8_t> frame;
  frame.reserve(kSealedFrameOverhead + payload.size());
  frame.push_back(kSealedFrameMarker);
  for (std::size_t i = 0; i < 8; ++i) {
    frame.push_back(static_cast<std::uint8_t>(send_sequence_ >> (i * 8)));
  }
  const std::vector<std::uint8_t> ciphertext =
      ChaCha20Xor(send_key_, SequenceNonce(send_sequence_), 0, payload);
  frame.insert(frame.end(), ciphertext.begin(), ciphertext.end());

  const Sha256Digest tag = HmacSha256(AsBytes(send_mac_), frame);
  frame.insert(frame.end(), tag.begin(), tag.end());
  ++send_sequence_;
  return frame;
}

auto SessionCipher::Open(std::span<const std::uint8_t> frame,
                         std::vector<std::uint8_t>& payload) -> bool {
  constexpr std::size_t header_size = 1 + 8;
  if (frame.size() < header_size + kHashSize ||
      frame[0] != kSealedFrameMarker) {
    return false;
  }

  std::uint64_t sequence = 0;
  for (std::size_t i = 0; i < 8; ++i) {
    sequence |= static_cast<std::uint64_t>(frame[1 + i]) << (i * 8);
  }
  if (sequence != receive_sequence_) {
    return false;
  }

  const std::span<const std::uint8_t> signed_part =
      frame.first(frame.size() - kHashSize);
  const Sha256Digest expected = HmacSha256(AsBytes(receive_mac_), signed_part);
  if (!TagMatches(AsBytes(expected), frame.last(kHashSize))) {
    return false;
  }

  const std::span<const std::uint8_t> ciphertext =
      frame.subspan(header_size, frame.size() - header_size - kHashSize);
  payload = ChaCha20Xor(receive_key_, SequenceNonce(sequence), 0, ciphertext);
  ++receive_sequence_;
  return true;
}

}  // namespace dnd
