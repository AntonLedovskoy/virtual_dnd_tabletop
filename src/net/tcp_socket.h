#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace dnd {

// TCP-фрейминг с префиксом длины поверх Winsock. Каждый фрейм — одно
// сообщение протокола; сокетный слой работает только с байтами.
auto InitializeSockets() -> bool;

// Не-loopback IPv4-адреса машины — для показа адреса хоста в лобби.
auto LocalIpv4Addresses() -> std::vector<std::string>;

class TcpConnection {
 public:
  ~TcpConnection();
  TcpConnection(const TcpConnection&) = delete;
  auto operator=(const TcpConnection&) -> TcpConnection& = delete;

  // Неблокирующее подключение: соединение может ещё устанавливаться;
  // Poll() сообщает о прогрессе, никогда не блокируя вызывающего.
  static auto Connect(const std::string& host, std::uint16_t port)
      -> std::unique_ptr<TcpConnection>;

  auto QueueFrame(const std::vector<std::uint8_t>& frame) -> void;
  // Читает доступные данные и добавляет целые фреймы. Возвращает false
  // после отключения собеседника или ошибки ввода-вывода.
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

  // Порт 0 просит ОС выделить временный порт (используется в тестах).
  auto Listen(std::uint16_t port) -> bool;
  [[nodiscard]] auto Listening() const -> bool;
  [[nodiscard]] auto Port() const -> std::uint16_t;
  [[nodiscard]] auto Accept() const -> std::unique_ptr<TcpConnection>;

 private:
  std::uintptr_t handle_ = static_cast<std::uintptr_t>(-1);
};

}  // namespace dnd
