#include "net/tcp_socket.h"

#include <algorithm>
#include <array>
#include <utility>

#include "core/session_crypto.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#error "TCP networking is currently implemented for Windows only"
#endif

namespace dnd {
namespace {

constexpr std::uint32_t kMaxFrame = 16U * 1024U * 1024U;
constexpr std::uintptr_t kInvalidHandle = static_cast<std::uintptr_t>(~0ULL);
constexpr std::size_t kFrameHeaderSize = 4;
constexpr SOCKET kInvalidSocket = INVALID_SOCKET;

#ifdef _WIN32
auto ToSocket(std::uintptr_t handle) -> SOCKET {
  return static_cast<SOCKET>(handle);
}
#endif

auto SetNonBlocking(SOCKET socket_handle) -> void {
  u_long mode = 1;
  ioctlsocket(socket_handle, FIONBIO, &mode);
}

// Настройки уже установленного соединения: неблокирующий режим и отключение
// алгоритма Нейгла. Команды и события маленькие, и без TCP_NODELAY стек ждёт
// накопления пакета — ходы доходят с заметной задержкой.
auto ConfigureSocket(SOCKET socket_handle) -> void {
  SetNonBlocking(socket_handle);
  BOOL no_delay = TRUE;
  setsockopt(socket_handle, IPPROTO_TCP, TCP_NODELAY,
             reinterpret_cast<const char*>(&no_delay), sizeof(no_delay));
}

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

}  // namespace

auto InitializeSockets() -> bool {
#ifdef _WIN32
  static const bool initialized = []() -> bool {
    WSADATA data{};
    return WSAStartup(MAKEWORD(2, 2), &data) == 0;
  }();
  return initialized;
#else
  return false;
#endif
}

auto LocalIpv4Addresses() -> std::vector<std::string> {
#ifdef _WIN32
  if (!InitializeSockets()) {
    return {};
  }
  std::array<char, 256> hostname{};
  if (gethostname(hostname.data(), hostname.size()) != 0) {
    return {};
  }
  addrinfo hints{};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo* results = nullptr;
  if (getaddrinfo(hostname.data(), nullptr, &hints, &results) != 0) {
    return {};
  }
  std::vector<std::string> addresses;
  for (const addrinfo* entry = results; entry != nullptr;
       entry = entry->ai_next) {
    const auto* address = reinterpret_cast<const sockaddr_in*>(entry->ai_addr);
    std::array<char, INET_ADDRSTRLEN> text{};
    if (inet_ntop(AF_INET, &address->sin_addr, text.data(), text.size()) ==
        nullptr) {
      continue;
    }
    std::string value(text.data());
    if (value == "127.0.0.1" || value.starts_with("169.254.")) {
      continue;
    }
    if (std::ranges::find(addresses, value) == addresses.end()) {
      addresses.push_back(std::move(value));
    }
  }
  freeaddrinfo(results);
  return addresses;
#else
  return {};
#endif
}

TcpConnection::TcpConnection(std::uintptr_t handle, bool connecting)
    : handle_(handle), connected_(!connecting), connecting_(connecting) {
  ConfigureSocket(ToSocket(handle_));
}

TcpConnection::~TcpConnection() {
  if (handle_ != kInvalidHandle) {
    closesocket(ToSocket(handle_));
  }
}

auto TcpConnection::Connect(const std::string& host, std::uint16_t port)
    -> std::unique_ptr<TcpConnection> {
  if (!InitializeSockets()) {
    return nullptr;
  }
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;

  const std::string port_text = std::to_string(port);
  addrinfo* addresses = nullptr;
  if (getaddrinfo(host.c_str(), port_text.c_str(), &hints, &addresses) != 0) {
    return nullptr;
  }

  std::unique_ptr<TcpConnection> connection;
  for (const addrinfo* address = addresses; address != nullptr;
       address = address->ai_next) {
    const SOCKET socket_handle =
        socket(address->ai_family, address->ai_socktype, address->ai_protocol);
    if (socket_handle == kInvalidSocket) {
      continue;
    }
    ConfigureSocket(socket_handle);
    const int result = connect(socket_handle, address->ai_addr,
                               static_cast<int>(address->ai_addrlen));
    if (result == 0) {
      connection = std::unique_ptr<TcpConnection>(
          new TcpConnection(static_cast<std::uintptr_t>(socket_handle)));
      break;
    }
    const int error = WSAGetLastError();
    if (error == WSAEWOULDBLOCK || error == WSAEINPROGRESS ||
        error == WSAEALREADY) {
      connection = std::unique_ptr<TcpConnection>(
          new TcpConnection(static_cast<std::uintptr_t>(socket_handle), true));
      break;
    }
    closesocket(socket_handle);
  }
  freeaddrinfo(addresses);
  return connection;
}

auto TcpConnection::QueueFrame(const std::vector<std::uint8_t>& frame) -> void {
  if (!connected_ && !connecting_) {
    return;  // соединение уже закрыто: копить нечего
  }
  std::vector<std::uint8_t> wire =
      cipher_ != nullptr ? cipher_->Seal(frame) : frame;
  if (outgoing_.size() + kFrameHeaderSize + wire.size() > kMaxOutgoingBytes) {
    // Собеседник не читает: соединение закрывается, чтобы очередь не росла
    // до исчерпания памяти хоста.
    connected_ = false;
    outgoing_.clear();
    return;
  }
  AppendFrameSize(outgoing_, static_cast<std::uint32_t>(wire.size()));
  outgoing_.insert(outgoing_.end(), wire.begin(), wire.end());
  Flush();
}

auto TcpConnection::SetSessionCipher(std::unique_ptr<SessionCipher> cipher)
    -> void {
  cipher_ = std::move(cipher);
}

auto TcpConnection::Close() -> void {
  connected_ = false;
  incoming_.clear();
  outgoing_.clear();
}

auto TcpConnection::Flush() -> bool {
  if (connecting_) {
    return true;
  }
  while (!outgoing_.empty()) {
    const int sent =
        send(ToSocket(handle_), reinterpret_cast<const char*>(outgoing_.data()),
             static_cast<int>(outgoing_.size()), 0);
    if (sent > 0) {
      outgoing_.erase(outgoing_.begin(), outgoing_.begin() + sent);
      continue;
    }
    if (sent == 0 || WSAGetLastError() != WSAEWOULDBLOCK) {
      connected_ = false;
      return false;
    }
    break;
  }
  return true;
}

auto TcpConnection::TryCompleteConnect() -> bool {
  if (!connecting_) {
    return true;
  }
  fd_set writable;
  FD_ZERO(&writable);
  FD_SET(ToSocket(handle_), &writable);
  const timeval timeout{};
  const int ready = select(0, nullptr, &writable, nullptr, &timeout);
  if (ready == 0) {
    return true;  // ещё подключается, повторим в следующем кадре
  }
  if (ready < 0) {
    connecting_ = false;
    return false;
  }
  int error = 0;
  int error_size = static_cast<int>(sizeof(error));
  if (getsockopt(ToSocket(handle_), SOL_SOCKET, SO_ERROR,
                 reinterpret_cast<char*>(&error), &error_size) != 0 ||
      error != 0) {
    connecting_ = false;
    return false;
  }
  connecting_ = false;
  connected_ = true;
  return true;
}

auto TcpConnection::Poll(std::vector<ReceivedFrame>& frames) -> bool {
  if (!connected_) {
    if (!TryCompleteConnect()) {
      return false;
    }
    if (connecting_) {
      return true;
    }
  }
  std::array<std::uint8_t, 16384> buffer{};
  for (;;) {
    const int received =
        recv(ToSocket(handle_), reinterpret_cast<char*>(buffer.data()),
             static_cast<int>(buffer.size()), 0);
    if (received > 0) {
      incoming_.insert(incoming_.end(), buffer.begin(),
                       buffer.begin() + received);
      continue;
    }
    if (received == 0) {
      connected_ = false;
      return false;
    }
    if (WSAGetLastError() != WSAEWOULDBLOCK) {
      connected_ = false;
      return false;
    }
    break;
  }

  ParseFrames(frames);
  return connected_ && Flush();
}

auto TcpConnection::ParseFrames(std::vector<ReceivedFrame>& frames) -> void {
  std::size_t offset = 0;
  while (incoming_.size() - offset >= kFrameHeaderSize) {
    const std::uint32_t size = ReadFrameSize(incoming_, offset);
    if (size > kMaxFrame) {
      Close();
      return;
    }
    if (incoming_.size() - offset - kFrameHeaderSize < size) {
      break;
    }
    const auto begin = incoming_.begin() +
                       static_cast<std::ptrdiff_t>(offset + kFrameHeaderSize);
    ReceivedFrame frame;
    if (size > 0 && *begin == kSealedFrameMarker) {
      // Шифрованный кадр до рукопожатия или с битым MAC — не разбираемся,
      // а рвём соединение: разбирать нечего и доверять нельзя.
      if (cipher_ == nullptr ||
          !cipher_->Open(std::span<const std::uint8_t>(
                             incoming_.data() + offset + kFrameHeaderSize,
                             static_cast<std::size_t>(size)),
                         frame.payload)) {
        Close();
        return;
      }
      frame.sealed = true;
    } else {
      frame.payload.assign(begin, begin + static_cast<std::ptrdiff_t>(size));
    }
    frames.push_back(std::move(frame));
    offset += kFrameHeaderSize + size;
  }
  incoming_.erase(incoming_.begin(),
                  incoming_.begin() + static_cast<std::ptrdiff_t>(offset));
}

TcpListener::~TcpListener() { Stop(); }

auto TcpListener::Stop() -> void {
  if (handle_ != kInvalidHandle) {
    closesocket(ToSocket(handle_));
    handle_ = kInvalidHandle;
  }
}

auto TcpListener::Listen(std::uint16_t port) -> bool {
  if (!InitializeSockets()) {
    return false;
  }
  const SOCKET socket_handle = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (socket_handle == kInvalidSocket) {
    return false;
  }

  BOOL reuse = TRUE;
  setsockopt(socket_handle, SOL_SOCKET, SO_REUSEADDR,
             reinterpret_cast<const char*>(&reuse), sizeof(reuse));

  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_ANY);
  address.sin_port = htons(port);

  if (bind(socket_handle, reinterpret_cast<sockaddr*>(&address),
           sizeof(address)) != 0) {
    closesocket(socket_handle);
    return false;
  }
  if (listen(socket_handle, SOMAXCONN) != 0) {
    closesocket(socket_handle);
    return false;
  }

  SetNonBlocking(socket_handle);
  handle_ = static_cast<std::uintptr_t>(socket_handle);
  return true;
}

auto TcpListener::Listening() const -> bool {
  return handle_ != kInvalidHandle;
}

auto TcpListener::Port() const -> std::uint16_t {
  if (!Listening()) {
    return 0;
  }
  sockaddr_in address{};
  int length = sizeof(address);
  if (getsockname(ToSocket(handle_), reinterpret_cast<sockaddr*>(&address),
                  &length) != 0) {
    return 0;
  }
  return ntohs(address.sin_port);
}

auto TcpListener::Accept() const -> std::unique_ptr<TcpConnection> {
  if (!Listening()) {
    return nullptr;
  }
  const SOCKET client = accept(ToSocket(handle_), nullptr, nullptr);
  if (client == kInvalidSocket) {
    return nullptr;
  }
  return std::unique_ptr<TcpConnection>(
      new TcpConnection(static_cast<std::uintptr_t>(client)));
}

}  // namespace dnd
