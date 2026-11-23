#include "net/tcp_socket.h"

#include <algorithm>
#include <array>
#include <boost/asio.hpp>
#include <cstddef>
#include <deque>
#include <iterator>
#include <utility>

#include "core/session_crypto.h"

namespace dnd {
namespace {

constexpr std::uint32_t kMaxFrame = 16U * 1024U * 1024U;
constexpr std::size_t kFrameHeaderSize = 4;
constexpr std::size_t kReadChunk = 16384;

auto ReadFrameSize(const std::vector<std::uint8_t>& buffer, std::size_t offset)
    -> std::uint32_t {
  return static_cast<std::uint32_t>(buffer[offset]) |
         (static_cast<std::uint32_t>(buffer[offset + 1]) << 8) |
         (static_cast<std::uint32_t>(buffer[offset + 2]) << 16) |
         (static_cast<std::uint32_t>(buffer[offset + 3]) << 24);
}

auto AppendFrameSize(std::vector<std::uint8_t>& buffer, std::uint32_t size)
    -> void {
  for (int shift = 0; shift < 32; shift += 8) {
    buffer.push_back(static_cast<std::uint8_t>((size >> shift) & 0xFFU));
  }
}

// Команды и события маленькие, и без TCP_NODELAY стек ждёт накопления
// пакета — ходы доходят с заметной задержкой.
auto ConfigureSocket(boost::asio::ip::tcp::socket& socket) -> void {
  boost::system::error_code error;
  (void)socket.set_option(boost::asio::ip::tcp::no_delay(true), error);
}

// Отменённые операции доставляются хендлерами: перед разрушением объекта
// прокачиваем очередь. Исключение хендлера в деструкторе выпускать нельзя.
auto Drain(boost::asio::io_context& io) -> void {
  try {
    io.poll();
  } catch (...) {
    return;
  }
}

}  // namespace

struct TcpConnection::Impl {
  boost::asio::io_context io;
  boost::asio::ip::tcp::socket socket{io};
  boost::asio::ip::tcp::resolver resolver{io};
  std::array<std::uint8_t, kReadChunk> read_buffer{};
  std::vector<std::uint8_t> incoming;
  // Кадры, ещё не отданные Asio.
  std::vector<std::uint8_t> outgoing;
  // Буфер записи в полёте: новые кадры копятся в outgoing, поэтому
  // push_back не инвалидирует буфер, переданный в async_write.
  std::vector<std::uint8_t> write_buffer;
  std::vector<ReceivedFrame> received;
  std::unique_ptr<SessionCipher> cipher;
  bool connected = false;
  bool connecting = false;
  bool writing = false;

  // Сокет и контекст не переживают соединение: отменённые операции
  // прокачиваются здесь, пока буферы и хендлеры ещё живы.
  ~Impl() {
    Close();
    Drain(io);
  }

  auto Start(const std::string& host, std::uint16_t port) -> void {
    connecting = true;
    resolver.async_resolve(
        host, std::to_string(port),
        [this](const boost::system::error_code& error,
               const boost::asio::ip::tcp::resolver::results_type& results)
            -> void {
          if (error) {
            connecting = false;
            return;
          }
          boost::asio::async_connect(
              socket, results,
              [this](
                  const boost::system::error_code& connect_error,
                  const boost::asio::ip::tcp::endpoint& /*endpoint*/) -> void {
                if (connect_error) {
                  connecting = false;
                  return;
                }
                connecting = false;
                connected = true;
                ConfigureSocket(socket);
                BeginRead();
                BeginWrite();
              });
        });
  }

  auto BeginRead() -> void {
    if (!connected) {
      return;
    }
    socket.async_read_some(
        boost::asio::buffer(read_buffer),
        [this](const boost::system::error_code& error,
               std::size_t size) -> void { HandleRead(error, size); });
  }

  auto HandleRead(const boost::system::error_code& error, std::size_t size)
      -> void {
    if (error) {
      if (error != boost::asio::error::operation_aborted) {
        connected = false;
      }
      return;
    }
    incoming.insert(incoming.end(), read_buffer.begin(),
                    read_buffer.begin() + static_cast<std::ptrdiff_t>(size));
    ParseFrames();
    BeginRead();
  }

  auto BeginWrite() -> void {
    if (!connected || writing || outgoing.empty()) {
      return;
    }
    write_buffer.swap(outgoing);
    writing = true;
    boost::asio::async_write(
        socket, boost::asio::buffer(write_buffer),
        [this](const boost::system::error_code& error,
               std::size_t /*size*/) -> void { HandleWrite(error); });
  }

  // Следующая порция уходит из Poll(): хендлер не должен вызывать начало
  // записи сам, иначе получается «рекурсивная» цепочка отмены/записи.
  auto HandleWrite(const boost::system::error_code& error) -> void {
    writing = false;
    write_buffer.clear();
    if (error && error != boost::asio::error::operation_aborted) {
      connected = false;
    }
  }

  auto ParseFrames() -> void {
    std::size_t offset = 0;
    while (incoming.size() - offset >= kFrameHeaderSize) {
      const std::uint32_t size = ReadFrameSize(incoming, offset);
      if (size > kMaxFrame) {
        Close();
        return;
      }
      if (incoming.size() - offset - kFrameHeaderSize < size) {
        break;
      }
      const auto begin = incoming.begin() +
                         static_cast<std::ptrdiff_t>(offset + kFrameHeaderSize);
      ReceivedFrame frame;
      if (size > 0 && *begin == kSealedFrameMarker) {
        // Шифрованный кадр до рукопожатия или с битым MAC — не разбираемся,
        // а рвём соединение: разбирать нечего и доверять нельзя.
        if (cipher == nullptr ||
            !cipher->Open(std::span<const std::uint8_t>(
                              incoming.data() + offset + kFrameHeaderSize,
                              static_cast<std::size_t>(size)),
                          frame.payload)) {
          Close();
          return;
        }
        frame.sealed = true;
      } else {
        frame.payload.assign(begin, begin + static_cast<std::ptrdiff_t>(size));
      }
      received.push_back(std::move(frame));
      offset += kFrameHeaderSize + size;
    }
    incoming.erase(incoming.begin(),
                   incoming.begin() + static_cast<std::ptrdiff_t>(offset));
  }

  auto Close() -> void {
    connected = false;
    connecting = false;
    boost::system::error_code error;
    (void)socket.close(error);
    incoming.clear();
    outgoing.clear();
  }

  auto QueueFrame(const std::vector<std::uint8_t>& frame) -> void {
    if (!connected && !connecting) {
      return;  // соединение уже закрыто: копить нечего
    }
    std::vector<std::uint8_t> wire =
        cipher != nullptr ? cipher->Seal(frame) : frame;
    if (write_buffer.size() + outgoing.size() + kFrameHeaderSize + wire.size() >
        kMaxOutgoingBytes) {
      // Собеседник не читает: соединение закрывается, чтобы очередь не росла
      // до исчерпания памяти хоста.
      Close();
      return;
    }
    AppendFrameSize(outgoing, static_cast<std::uint32_t>(wire.size()));
    outgoing.insert(outgoing.end(), wire.begin(), wire.end());
    BeginWrite();
  }
};

TcpConnection::TcpConnection() : impl_(std::make_unique<Impl>()) {}

TcpConnection::~TcpConnection() = default;

auto TcpConnection::Connect(const std::string& host, std::uint16_t port)
    -> std::unique_ptr<TcpConnection> {
  auto connection = std::unique_ptr<TcpConnection>(new TcpConnection());
  connection->impl_->Start(host, port);
  return connection;
}

auto TcpConnection::QueueFrame(const std::vector<std::uint8_t>& frame) -> void {
  impl_->QueueFrame(frame);
}

auto TcpConnection::Poll(std::vector<ReceivedFrame>& frames) -> bool {
  impl_->io.poll();
  // Запись, поставленная в очередь во время предыдущей, стартует здесь.
  impl_->BeginWrite();
  frames.insert(frames.end(), std::make_move_iterator(impl_->received.begin()),
                std::make_move_iterator(impl_->received.end()));
  impl_->received.clear();
  return impl_->connected || impl_->connecting;
}

auto TcpConnection::Connected() const -> bool {
  return impl_->connected || impl_->connecting;
}

auto TcpConnection::SetSessionCipher(std::unique_ptr<SessionCipher> cipher)
    -> void {
  impl_->cipher = std::move(cipher);
}

auto TcpConnection::Close() -> void { impl_->Close(); }

// Приём асинхронный: async_accept перевооружается после каждого соединения,
// а Accept() лишь прокачивает контекст и отдаёт уже принятые.
struct TcpListenerImpl {
  boost::asio::io_context io;
  boost::asio::ip::tcp::acceptor acceptor{io};
  std::deque<std::unique_ptr<TcpConnection>> pending;
  bool open = false;

  ~TcpListenerImpl() { Stop(); }

  auto Listen(std::uint16_t port) -> bool {
    Stop();
    boost::system::error_code error;
    (void)acceptor.open(boost::asio::ip::tcp::v4(), error);
    if (error) {
      return false;
    }
    // Перезапуск хоста в том же процессе должен занять тот же адрес.
    (void)acceptor.set_option(boost::asio::socket_base::reuse_address(true),
                              error);
    (void)acceptor.bind(
        boost::asio::ip::tcp::endpoint(boost::asio::ip::tcp::v4(), port),
        error);
    if (error) {
      acceptor.close();
      return false;
    }
    (void)acceptor.listen(boost::asio::socket_base::max_listen_connections,
                          error);
    if (error) {
      acceptor.close();
      return false;
    }
    open = true;
    BeginAccept();
    return true;
  }

  auto Stop() -> void {
    open = false;
    boost::system::error_code error;
    (void)acceptor.close(error);
    // Отмена async_accept доставляется хендлером: прокачиваем очередь,
    // пока слушатель жив, чтобы хендлер не остался на разрушенном объекте.
    Drain(io);
    pending.clear();
  }

  auto Accept() -> std::unique_ptr<TcpConnection> {
    io.poll();
    if (pending.empty()) {
      return nullptr;
    }
    std::unique_ptr<TcpConnection> connection = std::move(pending.front());
    pending.pop_front();
    return connection;
  }

  auto BeginAccept() -> void {
    if (!open) {
      return;
    }
    // Соединение создаётся заранее: async_accept принимает сокет в него,
    // у каждого соединения свой io_context.
    auto connection = std::unique_ptr<TcpConnection>(new TcpConnection());
    // Ссылка на сокет берётся до переноса connection в лямбду: порядок
    // вычисления аргументов в вызове не определён.
    boost::asio::ip::tcp::socket& peer_socket = connection->impl_->socket;
    acceptor.async_accept(
        peer_socket,
        [this, connection = std::move(connection)](
            const boost::system::error_code& error) mutable -> void {
          if (!error) {
            ConfigureSocket(connection->impl_->socket);
            connection->impl_->connected = true;
            connection->impl_->BeginRead();
            pending.push_back(std::move(connection));
          }
          BeginAccept();
        });
  }

  [[nodiscard]] auto Port() const -> std::uint16_t {
    boost::system::error_code error;
    const auto endpoint = acceptor.local_endpoint(error);
    return error ? 0 : endpoint.port();
  }
};

auto LocalIpv4Addresses() -> std::vector<std::string> {
  boost::asio::io_context io;
  boost::asio::ip::tcp::resolver resolver(io);
  boost::system::error_code error;
  const std::string host = boost::asio::ip::host_name(error);
  if (error) {
    return {};
  }
  const auto results = resolver.resolve(host, "", error);
  if (error) {
    return {};
  }
  std::vector<std::string> addresses;
  for (const auto& entry : results) {
    const boost::asio::ip::address address = entry.endpoint().address();
    if (!address.is_v4() || address.is_loopback()) {
      continue;
    }
    std::string value = address.to_string();
    if (value.starts_with("169.254.")) {
      continue;
    }
    if (std::ranges::find(addresses, value) == addresses.end()) {
      addresses.push_back(std::move(value));
    }
  }
  return addresses;
}

TcpListener::TcpListener() : impl_(std::make_unique<TcpListenerImpl>()) {}

TcpListener::~TcpListener() = default;

auto TcpListener::Listen(std::uint16_t port) -> bool {
  return impl_->Listen(port);
}

auto TcpListener::Stop() -> void { impl_->Stop(); }

auto TcpListener::Listening() const -> bool { return impl_->open; }

auto TcpListener::Port() const -> std::uint16_t { return impl_->Port(); }

auto TcpListener::Accept() -> std::unique_ptr<TcpConnection> {
  return impl_->Accept();
}

}  // namespace dnd
