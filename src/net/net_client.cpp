#include "net/net_client.h"

#include <utility>

namespace dnd {
namespace {

constexpr std::chrono::seconds kPingInterval{5};

}  // namespace

auto NetClient::Connect(const std::string& host, std::uint16_t port,
                        const std::string& player_name,
                        const std::string& password) -> bool {
  host_ = host;
  port_ = port;
  player_name_ = player_name;
  password_ = password;
  welcomed_ = false;
  hello_sent_ = false;
  last_ping_ = std::chrono::steady_clock::now();
  connection_ = TcpConnection::Connect(host_, port_);
  return connection_ != nullptr;
}

auto NetClient::Reconnect() -> bool {
  connection_.reset();
  return Connect(host_, port_, player_name_, password_);
}

auto NetClient::Disconnect() -> void {
  connection_.reset();
  welcomed_ = false;
  hello_sent_ = false;
}

auto NetClient::Connected() const -> bool {
  return connection_ != nullptr && connection_->Connected();
}

auto NetClient::SendCommand(const Command& command) -> void {
  if (connection_ != nullptr && welcomed_) {
    connection_->QueueFrame(EncodeCommandMessage(command));
  }
}

auto NetClient::Poll(NetClientUpdate& update) -> void {
  if (connection_ == nullptr) {
    return;
  }

  if (connection_->Connected() && !hello_sent_) {
    connection_->QueueFrame(
        EncodeHello(Hello{.player_name = player_name_, .password = password_}));
    hello_sent_ = true;
  }

  const auto now = std::chrono::steady_clock::now();
  if (now - last_ping_ >= kPingInterval && connection_->Connected()) {
    last_ping_ = now;
    connection_->QueueFrame(EncodePing());
  }

  std::vector<std::vector<std::uint8_t>> frames;
  if (!connection_->Poll(frames)) {
    update.disconnected = true;
    return;
  }

  for (const std::vector<std::uint8_t>& frame : frames) {
    if (frame.empty()) {
      continue;
    }
    const auto type = static_cast<MessageType>(frame[0]);

    if (type == MessageType::kWelcome) {
      std::optional<Welcome> welcome =
          DecodeWelcome(frame.data(), frame.size());
      if (welcome.has_value()) {
        welcomed_ = true;
        update.welcome = std::move(welcome);
      }
    } else if (type == MessageType::kEvent) {
      std::optional<Event> event =
          DecodeEventMessage(frame.data(), frame.size());
      if (event.has_value()) {
        update.events.push_back(std::move(*event));
      }
    } else if (type == MessageType::kRoster) {
      std::optional<std::vector<RosterEntry>> roster =
          DecodeRoster(frame.data(), frame.size());
      if (roster.has_value()) {
        update.roster = std::move(roster);
      }
    } else if (type == MessageType::kReject) {
      std::optional<std::string> reason =
          DecodeReject(frame.data(), frame.size());
      if (reason.has_value()) {
        update.reject = std::move(reason);
      }
    }
  }
}

}  // namespace dnd
