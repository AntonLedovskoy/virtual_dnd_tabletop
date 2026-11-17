#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "core/command.h"
#include "core/event.h"
#include "net/protocol.h"
#include "net/tcp_socket.h"

namespace dnd {

using PeerId = std::uint32_t;

struct PeerJoined {
  PeerId peer = 0;
  std::string name;
};

struct PeerInfo {
  PeerId id = 0;
  std::string name;
};

struct PeerCommand {
  PeerId peer = 0;
  std::string player_name;
  Command command;
};

// Принимает клиентов игроков. Авторитетный GameServer принадлежит приложению:
// оно передаёт сюда welcome/события и забирает команды участников.
class NetServer {
 public:
  NetServer() = default;
  ~NetServer() = default;
  NetServer(const NetServer&) = delete;
  auto operator=(const NetServer&) -> NetServer& = delete;

  auto Start(std::uint16_t port, const std::string& password = {}) -> bool;
  [[nodiscard]] auto Port() const -> std::uint16_t { return listener_.Port(); }
  [[nodiscard]] auto PeerCount() const -> std::size_t { return peers_.size(); }
  [[nodiscard]] auto Peers() const -> std::vector<PeerInfo>;
  // Удобный метод для хоста: отключает участника с точно таким именем.
  auto KickByName(const std::string& name) -> bool;

  // Принимает новые подключения, завершает Hello-handshake, собирает
  // корректные команды и отключает простаивающих. Вошедшим нужно отправить
  // Welcome.
  auto Poll(std::vector<PeerJoined>& joined, std::vector<PeerCommand>& commands,
            std::vector<PeerId>& dropped) -> void;

  auto SendWelcome(PeerId peer, const Welcome& welcome) -> void;
  auto BroadcastEvent(const Event& event) -> void;
  auto Reject(PeerId peer, const std::string& reason) -> void;

 private:
  struct Peer {
    PeerId id = 0;
    std::string name;
    bool welcomed = false;
    std::chrono::steady_clock::time_point last_seen =
        std::chrono::steady_clock::now();
    std::unique_ptr<TcpConnection> connection;
  };

  auto FindPeer(PeerId id) -> Peer*;
  auto HandleFrame(Peer& peer, const std::vector<std::uint8_t>& frame,
                   std::vector<PeerJoined>& joined,
                   std::vector<PeerCommand>& commands) -> void;
  [[nodiscard]] auto Roster() const -> std::vector<RosterEntry>;
  auto BroadcastRoster() -> void;

  TcpListener listener_;
  std::vector<Peer> peers_;
  PeerId next_peer_id_ = 1;
  std::string password_;
  std::vector<std::string> banned_names_;
  bool roster_dirty_ = false;
};

}  // namespace dnd
