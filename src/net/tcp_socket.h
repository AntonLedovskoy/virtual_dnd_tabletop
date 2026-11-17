#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace dnd {

// Length-prefixed TCP framing on top of Winsock. Every frame is a single
// protocol message; the socket layer only deals with byte blobs.
auto InitializeSockets() -> bool;

// Non-loopback IPv4 addresses of this machine, useful for showing the host
// address in the lobby.
auto LocalIpv4Addresses() -> std::vector<std::string>;

class TcpConnection {
 public:
  ~TcpConnection();
  TcpConnection(const TcpConnection&) = delete;
  auto operator=(const TcpConnection&) -> TcpConnection& = delete;

  // Non-blocking connect: returns a connection that may still be connecting;
  // Poll() reports progress without ever blocking the caller.
  static auto Connect(const std::string& host, std::uint16_t port)
      -> std::unique_ptr<TcpConnection>;

  auto QueueFrame(const std::vector<std::uint8_t>& frame) -> void;
  // Reads whatever is available and appends complete frames. Returns false
  // after the peer disconnected or an I/O error happened.
  auto Poll(std::vector<std::vector<std::uint8_t>>& frames) -> bool;
  [[nodiscard]] auto Connected() const -> bool { return connected_; }

 private:
  friend class TcpListener;
  explicit TcpConnection(std::uintptr_t handle, bool connecting = false);

  auto Flush() -> bool;
  auto TryCompleteConnect() -> bool;
  auto ParseFrames(std::vector<std::vector<std::uint8_t>>& frames) -> void;

  std::uintptr_t handle_;
  bool connected_ = true;
  bool connecting_ = false;
  std::vector<std::uint8_t> incoming_;
  std::vector<std::uint8_t> outgoing_;
};

class TcpListener {
 public:
  TcpListener() = default;
  ~TcpListener();
  TcpListener(const TcpListener&) = delete;
  auto operator=(const TcpListener&) -> TcpListener& = delete;

  // port 0 asks the OS for an ephemeral port (used by tests).
  auto Listen(std::uint16_t port) -> bool;
  [[nodiscard]] auto Listening() const -> bool;
  [[nodiscard]] auto Port() const -> std::uint16_t;
  [[nodiscard]] auto Accept() const -> std::unique_ptr<TcpConnection>;

 private:
  std::uintptr_t handle_ = static_cast<std::uintptr_t>(-1);
};

}  // namespace dnd
