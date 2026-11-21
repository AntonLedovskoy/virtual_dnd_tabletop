#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "core/command.h"
#include "core/event.h"
#include "core/password.h"
#include "core/snapshot.h"
#include "core/visibility.h"
#include "core/x25519.h"

namespace dnd {

inline constexpr std::uint32_t kProtocolVersion = 32;

enum class MessageType : std::uint8_t {
  kHello = 1,
  kWelcome = 2,
  kCommand = 3,
  kEvent = 4,
  kReject = 5,
  kPing = 6,
  kRoster = 7,
  kChallenge = 8,
  kSessionReady = 9,
};

// Запрос хоста: случайный nonce привязывает отпечаток пароля к сессии, а
// открытый ключ — начало обмена ключами шифрования.
struct Challenge {
  ChallengeNonce nonce{};
  X25519Key public_key{};
};

// Пароль по сети не ходит: клиент доказывает знание пароля отпечатком
// HMAC-SHA256 по транскрипту рукопожатия (запрос, оба ключа, имя).
struct Hello {
  std::string player_name;
  X25519Key public_key{};
  PasswordProof proof{};
};

// Ответ хоста на верный отпечаток: он доказывает уже клиенту, что ключи
// не подменены. Уходит в зашифрованном кадре.
struct SessionReady {
  PasswordProof proof{};
};

// Читает только начало Hello: тип сообщения и версию протокола. Раскладка
// этих пяти байт не меняется между версиями, поэтому сервер может отличить
// чужую версию от битого пакета и ответить понятным отказом.
auto DecodeHelloVersion(const std::uint8_t* data, std::size_t size)
    -> std::optional<std::uint32_t>;

// Список игроков для лобби и игры. Хост всегда первый и помечен как DM.
struct RosterEntry {
  std::string name;
  bool is_dm = false;
};

struct Welcome {
  GameSnapshot snapshot;
  FogSnapshot fog;
  bool is_dm = false;
};

auto EncodeHello(const Hello& hello) -> std::vector<std::uint8_t>;
auto DecodeHello(const std::uint8_t* data, std::size_t size)
    -> std::optional<Hello>;

auto EncodeChallenge(const Challenge& challenge) -> std::vector<std::uint8_t>;
auto DecodeChallenge(const std::uint8_t* data, std::size_t size)
    -> std::optional<Challenge>;

auto EncodeSessionReady(const SessionReady& ready) -> std::vector<std::uint8_t>;
auto DecodeSessionReady(const std::uint8_t* data, std::size_t size)
    -> std::optional<SessionReady>;

auto EncodeWelcome(const Welcome& welcome) -> std::vector<std::uint8_t>;
auto DecodeWelcome(const std::uint8_t* data, std::size_t size)
    -> std::optional<Welcome>;

auto EncodeCommandMessage(const Command& command) -> std::vector<std::uint8_t>;
auto DecodeCommandMessage(const std::uint8_t* data, std::size_t size)
    -> std::optional<Command>;

auto EncodeEventMessage(const Event& event) -> std::vector<std::uint8_t>;
auto DecodeEventMessage(const std::uint8_t* data, std::size_t size)
    -> std::optional<Event>;

auto EncodeReject(const std::string& reason) -> std::vector<std::uint8_t>;
auto DecodeReject(const std::uint8_t* data, std::size_t size)
    -> std::optional<std::string>;

auto EncodeRoster(const std::vector<RosterEntry>& roster)
    -> std::vector<std::uint8_t>;
auto DecodeRoster(const std::uint8_t* data, std::size_t size)
    -> std::optional<std::vector<RosterEntry>>;

// Heartbeat: не даёт серверу отключить соединение по таймауту.
auto EncodePing() -> std::vector<std::uint8_t>;
auto DecodePing(const std::uint8_t* data, std::size_t size) -> bool;

}  // namespace dnd
