#include "net/net_server.h"

#include <algorithm>
#include <utility>

#include "core/player_name.h"
#include "core/session_crypto.h"

namespace dnd {
namespace {

constexpr std::chrono::seconds kIdleTimeout{15};
// Сколько живёт соединение, которому отправлен отказ: клиент должен успеть
// прочитать причину, но место на столе такие подключения не занимают.
constexpr std::chrono::seconds kFarewellDelay{2};
// Сколько отказных соединений хост держит одновременно. Всё сверх этого
// закрывается сразу: иначе поток подключений съел бы память.
constexpr std::size_t kFarewellPeers = 8;

}  // namespace

auto NetServer::Peer::AllowMessage(std::chrono::steady_clock::time_point now)
    -> bool {
  if (now - rate_window_start >= std::chrono::seconds(1)) {
    rate_window_start = now;
    rate_commands = 0;
  }
  if (rate_commands >= kMaxCommandsPerSecond) {
    return false;
  }
  ++rate_commands;
  return true;
}

auto NetServer::Start(std::uint16_t port, const std::string& password) -> bool {
  password_ = password;
  roster_dirty_ = true;
  return listener_.Listen(port);
}

auto NetServer::Stop() -> void {
  // Соединения закрываются вместе с объектами Peer; порт освобождается сразу,
  // чтобы новый хост в том же процессе занял тот же адрес.
  peers_.clear();
  listener_.Stop();
  next_peer_id_ = 1;
  banned_names_.clear();
  roster_dirty_ = false;
}

auto NetServer::Poll(std::vector<PeerJoined>& joined,
                     std::vector<PeerCommand>& commands,
                     std::vector<PeerId>& dropped) -> void {
  while (std::unique_ptr<TcpConnection> connection = listener_.Accept()) {
    if (peers_.size() >= kMaxPeers + kFarewellPeers) {
      continue;  // совсем нет места: соединение закрывается сразу
    }
    Peer peer;
    peer.id = next_peer_id_++;
    peer.connection = std::move(connection);
    const auto active = static_cast<std::size_t>(std::ranges::count_if(
        peers_, [](const Peer& other) -> bool { return !other.farewell; }));
    if (active >= kMaxPeers) {
      // Мест нет: причину надо успеть отправить, поэтому соединение живёт
      // ещё мгновение и закрывается по таймеру, а не рвётся сразу с RST.
      peer.farewell = true;
      peer.close_at = std::chrono::steady_clock::now() + kFarewellDelay;
      peer.connection->QueueFrame(EncodeReject("на хосте нет свободных мест"));
    } else {
      // Пароль по сети не ходит: сначала отдаём случайный запрос, на который
      // клиент отвечает отпечатком; вместе с ним — ключ хоста для шифрования
      // всего дальнейшего трафика.
      peer.ephemeral = GenerateX25519KeyPair();
      peer.challenge = Challenge{.nonce = RandomChallenge(),
                                 .public_key = peer.ephemeral.public_key};
      peer.challenge_sent = true;
      peer.connection->QueueFrame(EncodeChallenge(peer.challenge));
    }
    peers_.push_back(std::move(peer));
  }

  const auto now = std::chrono::steady_clock::now();
  for (Peer& peer : peers_) {
    std::vector<ReceivedFrame> frames;
    if (!peer.connection->Poll(frames)) {
      continue;
    }
    if (!frames.empty()) {
      peer.last_seen = now;
    }
    if (peer.farewell) {
      continue;  // отказ уже отправлен, читать нечего
    }
    for (const ReceivedFrame& frame : frames) {
      HandleFrame(peer, frame, joined, commands);
    }
  }

  for (auto it = peers_.begin(); it != peers_.end();) {
    const bool idle = !it->farewell && now - it->last_seen > kIdleTimeout;
    const bool expired = it->farewell && now >= it->close_at;
    if (!it->connection->Connected() || idle || expired) {
      if (it->welcomed) {
        dropped.push_back(it->id);
        roster_dirty_ = true;
      }
      it = peers_.erase(it);
    } else {
      ++it;
    }
  }

  if (roster_dirty_) {
    BroadcastRoster();
    roster_dirty_ = false;
  }
}

auto NetServer::HandleFrame(Peer& peer, const ReceivedFrame& frame,
                            std::vector<PeerJoined>& joined,
                            std::vector<PeerCommand>& commands) -> void {
  if (peer.session_ready && !frame.sealed) {
    // После рукопожатия всё шифруется: открытый кадр — попытка подмены.
    peer.connection->Close();
    return;
  }
  if (frame.payload.empty()) {
    return;
  }
  const auto type = static_cast<MessageType>(frame.payload[0]);

  if (type == MessageType::kPing) {
    return;
  }

  if (type == MessageType::kHello) {
    if (peer.welcomed) {
      return;
    }
    if (!peer.AllowMessage(std::chrono::steady_clock::now())) {
      // Перебор пароля: соединение закрывается, а не крутится в памяти.
      Reject(peer.id, "слишком много попыток входа");
      peer.farewell = true;
      peer.close_at = std::chrono::steady_clock::now() + kFarewellDelay;
      return;
    }
    // Сначала проверяем версию: о несовпадении надо сказать прямо, иначе
    // игрок со старой сборкой увидит только «некорректное приветствие».
    const std::optional<std::uint32_t> version =
        DecodeHelloVersion(frame.payload.data(), frame.payload.size());
    if (!version.has_value()) {
      Reject(peer.id, "некорректное приветствие");
      return;
    }
    if (*version != kProtocolVersion) {
      Reject(peer.id, "версия игры не совпадает: у хоста " +
                          std::to_string(kProtocolVersion) + ", у клиента " +
                          std::to_string(*version) + "; обновите игру");
      return;
    }
    const std::optional<Hello> hello =
        DecodeHello(frame.payload.data(), frame.payload.size());
    if (!hello.has_value() || !peer.challenge_sent) {
      Reject(peer.id, "некорректное приветствие");
      return;
    }
    if (hello->player_name.empty()) {
      Reject(peer.id, "пустое имя игрока");
      return;
    }
    if (SamePlayerName(hello->player_name, "DM")) {
      Reject(peer.id, "имя DM зарезервировано");
      return;
    }
    // Общий секрет и транскрипт: отпечаток подписывает оба ключа и имя,
    // поэтому подменить ключи посреди рукопожатия нельзя.
    X25519Key shared{};
    if (!X25519SharedSecret(peer.ephemeral.private_key, hello->public_key,
                            shared)) {
      Reject(peer.id, "некорректный ключ шифрования");
      return;
    }
    const std::vector<std::uint8_t> transcript =
        HandshakeTranscript(peer.challenge.nonce, peer.ephemeral.public_key,
                            hello->public_key, hello->player_name);
    if (!ProofMatches(MakeClientProof(password_, transcript), hello->proof)) {
      Reject(peer.id, "неверный пароль");
      return;
    }
    // Пароль доказан: дальше кадры идут только шифрованными ключами,
    // которые вывели оба участника.
    peer.connection->SetSessionCipher(
        std::make_unique<SessionCipher>(SessionCipher::ForHost(
            DeriveSessionSecrets(shared, peer.challenge.nonce))));
    peer.session_ready = true;
    peer.challenge_sent = false;
    peer.connection->QueueFrame(EncodeSessionReady(
        SessionReady{.proof = MakeServerProof(password_, transcript)}));
    // Запрет и склейка имён тоже без учёта регистра: иначе "Player" обходит
    // кик игрока "player" и садится рядом с ним под тем же вымышленным именем.
    const std::string key = PlayerNameKey(hello->player_name);
    if (std::ranges::find(banned_names_, key) != banned_names_.end()) {
      Reject(peer.id, "вы исключены из игры");
      return;
    }
    const bool duplicate =
        std::ranges::any_of(peers_, [&hello, &peer](const Peer& other) -> bool {
          return other.id != peer.id && other.welcomed &&
                 other.connection->Connected() &&
                 SamePlayerName(other.name, hello->player_name);
        });
    if (duplicate) {
      Reject(peer.id, "имя игрока уже занято");
      return;
    }
    peer.name = hello->player_name;
    peer.welcomed = true;
    peer.challenge_sent = false;  // повторно Hello не принимается
    roster_dirty_ = true;
    joined.push_back(PeerJoined{.peer = peer.id, .name = peer.name});
    return;
  }

  if (type == MessageType::kCommand) {
    if (!peer.welcomed) {
      return;
    }
    if (!peer.AllowMessage(std::chrono::steady_clock::now())) {
      return;  // слишком часто: молча отбрасываем
    }
    const std::optional<Command> command =
        DecodeCommandMessage(frame.payload.data(), frame.payload.size());
    if (!command.has_value()) {
      Reject(peer.id, "некорректная команда");
      return;
    }
    commands.push_back(PeerCommand{
        .peer = peer.id,
        .player_name = peer.name,
        .command = *command,
    });
  }
}

auto NetServer::FindPeer(PeerId id) -> Peer* {
  for (Peer& peer : peers_) {
    if (peer.id == id) {
      return &peer;
    }
  }
  return nullptr;
}

auto NetServer::SendWelcome(PeerId peer_id, const Welcome& welcome) -> void {
  Peer* peer = FindPeer(peer_id);
  if (peer != nullptr) {
    peer->connection->QueueFrame(EncodeWelcome(welcome));
  }
}

auto NetServer::SendEvent(PeerId peer_id, const Event& event) -> void {
  Peer* peer = FindPeer(peer_id);
  if (peer != nullptr && peer->welcomed) {
    peer->connection->QueueFrame(EncodeEventMessage(event));
  }
}

auto NetServer::BroadcastEvent(const Event& event) -> void {
  const std::vector<std::uint8_t> frame = EncodeEventMessage(event);
  for (Peer& peer : peers_) {
    if (peer.welcomed) {
      peer.connection->QueueFrame(frame);
    }
  }
}

auto NetServer::Reject(PeerId peer_id, const std::string& reason) -> void {
  Peer* peer = FindPeer(peer_id);
  if (peer != nullptr) {
    peer->connection->QueueFrame(EncodeReject(reason));
  }
}

auto NetServer::Peers() const -> std::vector<PeerInfo> {
  std::vector<PeerInfo> result;
  for (const Peer& peer : peers_) {
    if (peer.welcomed) {
      result.push_back(PeerInfo{.id = peer.id, .name = peer.name});
    }
  }
  return result;
}

auto NetServer::KickByName(const std::string& name) -> bool {
  const auto it =
      std::ranges::find_if(peers_, [&name](const Peer& peer) -> bool {
        return peer.welcomed && SamePlayerName(peer.name, name);
      });
  if (it == peers_.end()) {
    return false;
  }
  banned_names_.push_back(PlayerNameKey(it->name));
  peers_.erase(it);
  roster_dirty_ = true;
  return true;
}

auto NetServer::Roster() const -> std::vector<RosterEntry> {
  std::vector<RosterEntry> roster;
  roster.push_back(RosterEntry{.name = "DM", .is_dm = true});
  for (const Peer& peer : peers_) {
    if (peer.welcomed) {
      roster.push_back(RosterEntry{.name = peer.name, .is_dm = false});
    }
  }
  return roster;
}

auto NetServer::BroadcastRoster() -> void {
  const std::vector<std::uint8_t> frame = EncodeRoster(Roster());
  for (Peer& peer : peers_) {
    if (peer.welcomed) {
      peer.connection->QueueFrame(frame);
    }
  }
}

}  // namespace dnd
