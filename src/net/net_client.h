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

// Клиентская половина TCP-протокола: отправляет Hello при подключении,
// пингует хост и отправляет команды; приложение передаёт события в ClientView.
class NetClient {
 public:
  NetClient() = default;
  ~NetClient() = default;
  NetClient(const NetClient&) = delete;
  auto operator=(const NetClient&) -> NetClient& = delete;

  auto Connect(const std::string& host, std::uint16_t port,
               const std::string& player_name, const std::string& password = {})
      -> bool;
  // Переподключается к тому же адресу после разрыва.
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
