#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace dnd {

// Размер случайного запроса, который хост присылает сразу после подключения,
// и размер отпечатка пароля в ответе клиента.
inline constexpr std::size_t kChallengeSize = 16;
inline constexpr std::size_t kProofSize = 32;

using ChallengeNonce = std::array<std::uint8_t, kChallengeSize>;
using PasswordProof = std::array<std::uint8_t, kProofSize>;

// Случайный запрос от системного генератора: он не должен повторяться, иначе
// подсмотренный ответ можно переиграть.
auto RandomChallenge() -> ChallengeNonce;

// Пароль по сети не ходит: клиент доказывает знание пароля отпечатком
// HMAC-SHA256(пароль, nonce).
auto MakePasswordProof(std::string_view password, const ChallengeNonce& nonce)
    -> PasswordProof;

// Сравнение отпечатков без раннего выхода: время ответа не зависит от того,
// на каком байте отпечатки разошлись.
auto ProofMatches(const PasswordProof& expected, const PasswordProof& actual)
    -> bool;

}  // namespace dnd
