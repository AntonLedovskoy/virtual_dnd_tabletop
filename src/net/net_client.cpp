#include "net/net_client.h"

#include <utility>

#include "core/session_crypto.h"

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
  session_ready_ = false;
  transcript_.clear();
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
  session_ready_ = false;
  transcript_.clear();
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

  const auto now = std::chrono::steady_clock::now();
  if (now - last_ping_ >= kPingInterval && connection_->Connected()) {
    last_ping_ = now;
    connection_->QueueFrame(EncodePing());
  }

  std::vector<ReceivedFrame> frames;
  if (!connection_->Poll(frames)) {
    update.disconnected = true;
    return;
  }

  for (const ReceivedFrame& frame : frames) {
    if (frame.payload.empty()) {
      continue;
    }
    const auto type = static_cast<MessageType>(frame.payload[0]);

    if (frame.sealed) {
      if (type == MessageType::kSessionReady) {
        // Хост доказывает, что ключи выведены из того же пароля: иначе
        // перехватчик мог бы подменить обмен ключами.
        const std::optional<SessionReady> ready =
            DecodeSessionReady(frame.payload.data(), frame.payload.size());
        if (!ready.has_value() ||
            !ProofMatches(MakeServerProof(password_, transcript_),
                          ready->proof)) {
          update.reject = "хост не подтвердил пароль";
          Disconnect();
          return;
        }
        session_ready_ = true;
        continue;
      }
      if (!session_ready_) {
        // Данные до подтверждения хоста принимать нельзя.
        update.reject = "хост нарушил порядок рукопожатия";
        Disconnect();
        return;
      }
    } else {
      if (session_ready_) {
        // После рукопожатия открытые кадры — попытка подмены.
        update.disconnected = true;
        Disconnect();
        return;
      }
      if (type != MessageType::kChallenge && type != MessageType::kReject &&
          type != MessageType::kVersionMismatch) {
        continue;  // открытые данные до рукопожатия не принимаем
      }
    }

    if (type == MessageType::kChallenge) {
      if (hello_sent_) {
        continue;
      }
      const std::optional<Challenge> challenge =
          DecodeChallenge(frame.payload.data(), frame.payload.size());
      if (!challenge.has_value()) {
        continue;
      }
      const X25519KeyPair keys = GenerateX25519KeyPair();
      X25519Key shared{};
      if (!X25519SharedSecret(keys.private_key, challenge->public_key,
                              shared)) {
        update.reject = "хост прислал некорректный ключ";
        Disconnect();
        return;
      }
      transcript_ = HandshakeTranscript(challenge->nonce, challenge->public_key,
                                        keys.public_key, player_name_);
      // Hello уходит открытым кадром: ключей у хоста ещё нет. Сразу после
      // него включаем шифрование — всё остальное хост шлёт запечатанным.
      connection_->QueueFrame(
          EncodeHello(Hello{.player_name = player_name_,
                            .public_key = keys.public_key,
                            .proof = MakeClientProof(password_, transcript_)}));
      connection_->SetSessionCipher(
          std::make_unique<SessionCipher>(SessionCipher::ForClient(
              DeriveSessionSecrets(shared, challenge->nonce))));
      hello_sent_ = true;
    } else if (type == MessageType::kWelcome) {
      std::optional<Welcome> welcome =
          DecodeWelcome(frame.payload.data(), frame.payload.size());
      if (welcome.has_value()) {
        welcomed_ = true;
        update.welcome = std::move(welcome);
      }
    } else if (type == MessageType::kEvent) {
      std::optional<Event> event =
          DecodeEventMessage(frame.payload.data(), frame.payload.size());
      if (event.has_value()) {
        update.events.push_back(std::move(*event));
      }
    } else if (type == MessageType::kRoster) {
      std::optional<std::vector<RosterEntry>> roster =
          DecodeRoster(frame.payload.data(), frame.payload.size());
      if (roster.has_value()) {
        update.roster = std::move(*roster);
      }
    } else if (type == MessageType::kReject) {
      std::optional<std::string> reason =
          DecodeReject(frame.payload.data(), frame.payload.size());
      if (reason.has_value()) {
        update.reject = std::move(*reason);
      }
    } else if (type == MessageType::kVersionMismatch) {
      std::optional<VersionMismatch> mismatch =
          DecodeVersionMismatch(frame.payload.data(), frame.payload.size());
      if (mismatch.has_value()) {
        update.mismatch = *mismatch;
      }
    }
  }
}

}  // namespace dnd
