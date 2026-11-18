#include "net/net_server.h"

#include <algorithm>
#include <utility>

namespace dnd {
namespace {

constexpr std::chrono::seconds kIdleTimeout{15};

}  // namespace

auto NetServer::Start(std::uint16_t port, const std::string& password) -> bool {
  password_ = password;
  roster_dirty_ = true;
  return listener_.Listen(port);
}

auto NetServer::Poll(std::vector<PeerJoined>& joined,
                     std::vector<PeerCommand>& commands,
                     std::vector<PeerId>& dropped) -> void {
  while (std::unique_ptr<TcpConnection> connection = listener_.Accept()) {
    Peer peer;
    peer.id = next_peer_id_++;
    peer.connection = std::move(connection);
    peers_.push_back(std::move(peer));
  }

  const auto now = std::chrono::steady_clock::now();
  for (Peer& peer : peers_) {
    std::vector<std::vector<std::uint8_t>> frames;
    if (!peer.connection->Poll(frames)) {
      continue;
    }
    if (!frames.empty()) {
      peer.last_seen = now;
    }
    for (const std::vector<std::uint8_t>& frame : frames) {
      HandleFrame(peer, frame, joined, commands);
    }
  }

  for (auto it = peers_.begin(); it != peers_.end();) {
    const bool idle = now - it->last_seen > kIdleTimeout;
    if (!it->connection->Connected() || idle) {
      dropped.push_back(it->id);
      it = peers_.erase(it);
      roster_dirty_ = true;
    } else {
      ++it;
    }
  }

  if (roster_dirty_) {
    BroadcastRoster();
    roster_dirty_ = false;
  }
}

auto NetServer::HandleFrame(Peer& peer, const std::vector<std::uint8_t>& frame,
                            std::vector<PeerJoined>& joined,
                            std::vector<PeerCommand>& commands) -> void {
  if (frame.empty()) {
    return;
  }
  const auto type = static_cast<MessageType>(frame[0]);

  if (type == MessageType::kPing) {
    return;
  }

  if (type == MessageType::kHello) {
    if (peer.welcomed) {
      return;
    }
    // Сначала проверяем версию: о несовпадении надо сказать прямо, иначе
    // игрок со старой сборкой увидит только «некорректное приветствие».
    const std::optional<std::uint32_t> version =
        DecodeHelloVersion(frame.data(), frame.size());
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
    const std::optional<Hello> hello = DecodeHello(frame.data(), frame.size());
    if (!hello.has_value()) {
      Reject(peer.id, "некорректное приветствие");
      return;
    }
    if (hello->player_name.empty()) {
      Reject(peer.id, "пустое имя игрока");
      return;
    }
    if (hello->player_name == "DM") {
      Reject(peer.id, "имя DM зарезервировано");
      return;
    }
    if (!password_.empty() && hello->password != password_) {
      Reject(peer.id, "неверный пароль");
      return;
    }
    if (std::ranges::find(banned_names_, hello->player_name) !=
        banned_names_.end()) {
      Reject(peer.id, "вы исключены из игры");
      return;
    }
    const bool duplicate =
        std::ranges::any_of(peers_, [&hello, &peer](const Peer& other) -> bool {
          return other.id != peer.id && other.welcomed &&
                 other.connection->Connected() &&
                 other.name == hello->player_name;
        });
    if (duplicate) {
      Reject(peer.id, "имя игрока уже занято");
      return;
    }
    peer.name = hello->player_name;
    peer.welcomed = true;
    roster_dirty_ = true;
    joined.push_back(PeerJoined{.peer = peer.id, .name = peer.name});
    return;
  }

  if (type == MessageType::kCommand) {
    if (!peer.welcomed) {
      return;
    }
    const std::optional<Command> command =
        DecodeCommandMessage(frame.data(), frame.size());
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
        return peer.welcomed && peer.name == name;
      });
  if (it == peers_.end()) {
    return false;
  }
  banned_names_.push_back(it->name);
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
