#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace dnd {

class SessionCipher;
struct TcpListenerImpl;

// Принятый кадр: содержимое протокола и признак того, был он зашифрован.
// Открытые кадры после рукопожатия недопустимы — так ловится попытка
// подсунуть незашифрованную команду.
struct ReceivedFrame {
  std::vector<std::uint8_t> payload;
  bool sealed = false;
};

// Предел очереди отправки. Если собеседник не читает данные и очередь
// переполняется, соединение закрывается: хост не должен копить память
// из-за одного медленного клиента.
inline constexpr std::size_t kMaxOutgoingBytes =
    static_cast<std::size_t>(1024) * 1024;

// Не-loopback IPv4-адреса машины — для показа адреса хоста в лобби.
auto LocalIpv4Addresses() -> std::vector<std::string>;

// TCP-фрейминг с префиксом длины поверх Boost.Asio. Каждый фрейм — одно
// сообщение протокола; сокетный слой работает только с байтами.
// Обмен полностью асинхронный: Poll() прокачивает внутренний io_context
// и отдаёт готовые кадры, никогда не блокируя вызывающего.
class TcpConnection {
 public:
  ~TcpConnection();
  TcpConnection(const TcpConnection&) = delete;
  auto operator=(const TcpConnection&) -> TcpConnection& = delete;

  // Подключение только запускается: адрес разрешается и соединение
  // устанавливается асинхронно, результат виден по Connected()/Poll().
  static auto Connect(const std::string& host, std::uint16_t port)
      -> std::unique_ptr<TcpConnection>;

  auto QueueFrame(const std::vector<std::uint8_t>& frame) -> void;
  // Прокачивает сетевые события и добавляет целые кадры. Возвращает false
  // после отключения собеседника или ошибки ввода-вывода.
  auto Poll(std::vector<ReceivedFrame>& frames) -> bool;
  // Подключение в процессе тоже считается живым: иначе клиент рвал бы
  // недоподключённый сокет по таймеру переподключения.
  [[nodiscard]] auto Connected() const -> bool;

  // С этого момента исходящие кадры шифруются, а открытые входящие
  // отвергаются: рукопожатие закончено.
  auto SetSessionCipher(std::unique_ptr<SessionCipher> cipher) -> void;
  // Аварийно закрывает соединение, не дожидаясь таймаута.
  auto Close() -> void;

 private:
  friend struct TcpListenerImpl;
  TcpConnection();

  struct Impl;
  std::unique_ptr<Impl> impl_;
};

class TcpListener {
 public:
  TcpListener();
  ~TcpListener();
  TcpListener(const TcpListener&) = delete;
  auto operator=(const TcpListener&) -> TcpListener& = delete;

  // Порт 0 просит ОС выделить временный порт (используется в тестах).
  auto Listen(std::uint16_t port) -> bool;
  // Закрывает слушающий сокет; Stop можно звать повторно.
  auto Stop() -> void;
  [[nodiscard]] auto Listening() const -> bool;
  [[nodiscard]] auto Port() const -> std::uint16_t;
  // Отдаёт соединение, принятое с момента прошлого вызова; приём идёт
  // асинхронно, поэтому готовое соединение появляется через кадр-другой.
  auto Accept() -> std::unique_ptr<TcpConnection>;

 private:
  std::unique_ptr<TcpListenerImpl> impl_;
};

}  // namespace dnd
