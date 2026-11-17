#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/command.h"
#include "core/event.h"
#include "net/protocol.h"
#include "net/tcp_socket.h"

namespace dnd {

struct NetClientUpdate {
  std::optional<Welcome> welcome;
  std::optional<std::string> reject;
  std::optional<std::vector<RosterEntry>> roster;
  std::vector<Event> events;
  bool disconnected = false;
};

// Client half of the TCP protocol: sends Hello on connect, pings the host and
// sends commands; the app feeds received events into its ClientView.
class NetClient {
 public:
  NetClient() = default;
  ~NetClient() = default;
  NetClient(const NetClient&) = delete;
  auto operator=(const NetClient&) -> NetClient& = delete;

  auto Connect(const std::string& host, std::uint16_t port,
               const std::string& player_name, const std::string& password = {})
      -> bool;
  // Re-establishes the connection to the same endpoint after a drop.
  auto Reconnect() -> bool;
  auto Disconnect() -> void;
  [[nodiscard]] auto Connected() const -> bool;
  [[nodiscard]] auto Welcomed() const -> bool { return welcomed_; }

  auto SendCommand(const Command& command) -> void;
  auto Poll(NetClientUpdate& update) -> void;

 private:
  std::unique_ptr<TcpConnection> connection_;
  std::string host_;
  std::string player_name_;
  std::string password_;
  std::uint16_t port_ = 0;
  bool welcomed_ = false;
  bool hello_sent_ = false;
  std::chrono::steady_clock::time_point last_ping_;
};

}  // namespace dnd
