#include <chrono>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <variant>
#include <vector>

#include "core/byte_io.h"
#include "core/game_state.h"
#include "core/grid_map.h"
#include "core/visibility.h"
#include "net/net_client.h"
#include "net/net_server.h"
#include "net/protocol.h"

namespace {

auto g_failures = 0;

auto Expect(bool condition, const char* name) -> void {
  if (!condition) {
    std::fprintf(stderr, "FAILED: %s\n", name);
    ++g_failures;
  }
}

auto CellAt(int x, int y) -> dnd::Cell { return dnd::Cell{.x = x, .y = y}; }

auto Now() -> std::chrono::steady_clock::time_point {
  return std::chrono::steady_clock::now();
}

auto MakeSnapshot() -> dnd::GameSnapshot {
  dnd::GridMap map(6, 4);
  map.SetBlocked(CellAt(3, 1), true);
  dnd::GameState state(map);
  (void)state.AddToken("hero", CellAt(1, 1));
  return state.Snapshot();
}

auto MakeFog(int width, int height) -> dnd::FogSnapshot {
  const dnd::FogOfWar fog(width, height);
  return fog.Snapshot();
}

auto TestTcpHandshakeAndEvents() -> void {
  dnd::NetServer server;
  Expect(server.Start(0), "server listens on an ephemeral port");
  const std::uint16_t port = server.Port();
  Expect(port != 0, "server reports its port");
  if (port == 0) {
    return;
  }

  dnd::NetClient client;
  Expect(client.Connect("127.0.0.1", port, "tester"),
         "client connection starts");

  std::vector<dnd::PeerJoined> joined;
  std::vector<dnd::PeerCommand> commands;
  std::vector<dnd::PeerId> dropped;
  dnd::NetClientUpdate update;
  const auto peer_deadline = Now() + std::chrono::seconds(3);
  while (joined.empty() && Now() < peer_deadline) {
    server.Poll(joined, commands, dropped);
    client.Poll(update);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  Expect(!joined.empty(), "server accepts the peer hello");
  if (joined.empty()) {
    return;
  }
  Expect(joined.front().name == "tester", "hello carries the player name");

  const dnd::GameSnapshot snapshot = MakeSnapshot();
  server.SendWelcome(
      joined.front().peer,
      dnd::Welcome{.snapshot = snapshot, .fog = MakeFog(6, 4), .is_dm = true});

  const auto welcome_deadline = Now() + std::chrono::seconds(3);
  while (!update.welcome.has_value() && Now() < welcome_deadline) {
    client.Poll(update);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  Expect(update.welcome.has_value(), "client receives the welcome snapshot");
  if (update.welcome.has_value()) {
    Expect(update.welcome->snapshot.tokens.size() == 1,
           "welcome carries the world tokens");
    Expect(update.welcome->snapshot.tokens.front().name == "hero",
           "welcome token data survives");
    Expect(update.welcome->fog.width == 6 && update.welcome->fog.height == 4,
           "welcome carries fog dimensions");
    Expect(update.welcome->is_dm, "welcome carries the role");
  }

  client.SendCommand(dnd::MoveToken{.id = 1, .target = CellAt(2, 2)});
  const auto command_deadline = Now() + std::chrono::seconds(3);
  while (commands.empty() && Now() < command_deadline) {
    server.Poll(joined, commands, dropped);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  Expect(commands.size() == 1, "server receives the client command");
  if (!commands.empty()) {
    Expect(std::holds_alternative<dnd::MoveToken>(commands.front().command),
           "command decodes to MoveToken");
  }

  commands.clear();
  client.SendCommand(dnd::Say{.text = "hi there"});
  const auto say_deadline = Now() + std::chrono::seconds(3);
  while (commands.empty() && Now() < say_deadline) {
    server.Poll(joined, commands, dropped);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  Expect(!commands.empty() &&
             std::holds_alternative<dnd::Say>(commands.front().command),
         "chat command survives the wire");

  server.BroadcastEvent(
      dnd::TokenMoved{.id = 1, .from = CellAt(1, 1), .to = CellAt(2, 2)});
  const auto event_deadline = Now() + std::chrono::seconds(3);
  while (update.events.empty() && Now() < event_deadline) {
    client.Poll(update);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  Expect(!update.events.empty(), "client receives a broadcast event");
  if (!update.events.empty()) {
    Expect(std::holds_alternative<dnd::TokenMoved>(update.events.front()),
           "broadcast decodes to TokenMoved");
  }
}

auto TestViewUpdateRoundtrip() -> void {
  const dnd::Event event = dnd::ViewUpdated{
      .snapshot = MakeSnapshot(),
      .fog = MakeFog(6, 4),
  };
  const std::vector<std::uint8_t> frame = dnd::EncodeEventMessage(event);
  const std::optional<dnd::Event> decoded =
      dnd::DecodeEventMessage(frame.data(), frame.size());
  Expect(decoded.has_value(), "view update decodes");
  if (decoded.has_value()) {
    Expect(std::holds_alternative<dnd::ViewUpdated>(*decoded),
           "view update keeps its type");
    const auto& view = std::get<dnd::ViewUpdated>(*decoded);
    Expect(view.snapshot.tokens.size() == 1, "view update carries tokens");
    Expect(view.fog.width == 6, "view update carries fog");
  }
}

auto TestPasswordAndRoster() -> void {
  dnd::NetServer server;
  Expect(server.Start(0, "secret"), "password server listens");
  const std::uint16_t port = server.Port();
  if (port == 0) {
    return;
  }

  dnd::NetClient wrong;
  Expect(wrong.Connect("127.0.0.1", port, "alice", "wrong"),
         "wrong-password client connects");
  std::vector<dnd::PeerJoined> wrong_joined;
  std::vector<dnd::PeerCommand> commands;
  std::vector<dnd::PeerId> dropped;
  dnd::NetClientUpdate wrong_update;
  const auto wrong_deadline = Now() + std::chrono::seconds(3);
  while (!wrong_update.reject.has_value() && Now() < wrong_deadline) {
    server.Poll(wrong_joined, commands, dropped);
    wrong.Poll(wrong_update);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  Expect(wrong_joined.empty(), "wrong password does not join");
  Expect(wrong_update.reject.has_value(), "wrong password is rejected");

  dnd::NetClient client;
  Expect(client.Connect("127.0.0.1", port, "alice", "secret"),
         "correct-password client connects");
  std::vector<dnd::PeerJoined> joined;
  dnd::NetClientUpdate update;
  const auto join_deadline = Now() + std::chrono::seconds(3);
  while (joined.empty() && Now() < join_deadline) {
    server.Poll(joined, commands, dropped);
    client.Poll(update);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  Expect(joined.size() == 1, "correct password joins");

  if (!joined.empty()) {
    server.SendWelcome(
        joined.front().peer,
        dnd::Welcome{
            .snapshot = MakeSnapshot(), .fog = MakeFog(6, 4), .is_dm = false});
  }

  bool saw_welcome = false;
  bool saw_roster = false;
  std::vector<dnd::RosterEntry> roster;
  const auto welcome_deadline = Now() + std::chrono::seconds(3);
  while ((!saw_welcome || !saw_roster) && Now() < welcome_deadline) {
    client.Poll(update);
    if (update.welcome.has_value()) {
      saw_welcome = true;
    }
    if (update.roster.has_value()) {
      roster = *update.roster;
      saw_roster = true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  Expect(saw_welcome, "password client receives the welcome");
  Expect(saw_roster, "password client receives the roster");

  bool saw_dm = false;
  bool saw_alice = false;
  for (const dnd::RosterEntry& entry : roster) {
    saw_dm = saw_dm || (entry.name == "DM" && entry.is_dm);
    saw_alice = saw_alice || (entry.name == "alice" && !entry.is_dm);
  }
  Expect(saw_dm, "roster contains the DM");
  Expect(saw_alice, "roster contains the joined player");

  Expect(server.KickByName("alice"), "host can kick a player");
  bool disconnected = false;
  const auto kick_deadline = Now() + std::chrono::seconds(3);
  while (!disconnected && Now() < kick_deadline) {
    client.Poll(update);
    disconnected = update.disconnected;
    server.Poll(joined, commands, dropped);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  Expect(disconnected, "kicked client is disconnected");

  dnd::NetClient kicked_again;
  Expect(kicked_again.Connect("127.0.0.1", port, "alice", "secret"),
         "kicked client can attempt to reconnect");
  dnd::NetClientUpdate kicked_update;
  const auto kicked_deadline = Now() + std::chrono::seconds(3);
  while (!kicked_update.reject.has_value() && Now() < kicked_deadline) {
    server.Poll(joined, commands, dropped);
    kicked_again.Poll(kicked_update);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  Expect(kicked_update.reject.has_value(), "kicked name cannot reconnect");
}

auto TestPlayerNameValidation() -> void {
  dnd::NetServer server;
  Expect(server.Start(0), "name test server listens");
  const std::uint16_t port = server.Port();
  if (port == 0) {
    return;
  }

  dnd::NetClient alice;
  Expect(alice.Connect("127.0.0.1", port, "alice"), "first client connects");
  std::vector<dnd::PeerJoined> joined;
  std::vector<dnd::PeerCommand> commands;
  std::vector<dnd::PeerId> dropped;
  dnd::NetClientUpdate alice_update;
  const auto deadline = Now() + std::chrono::seconds(3);
  while (joined.empty() && Now() < deadline) {
    server.Poll(joined, commands, dropped);
    alice.Poll(alice_update);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  Expect(joined.size() == 1, "unique player name is accepted");

  dnd::NetClient duplicate;
  Expect(duplicate.Connect("127.0.0.1", port, "alice"),
         "duplicate client connects");
  std::vector<dnd::PeerJoined> duplicate_joined;
  dnd::NetClientUpdate duplicate_update;
  const auto duplicate_deadline = Now() + std::chrono::seconds(3);
  while (duplicate_joined.empty() && !duplicate_update.reject.has_value() &&
         Now() < duplicate_deadline) {
    server.Poll(duplicate_joined, commands, dropped);
    duplicate.Poll(duplicate_update);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  Expect(duplicate_joined.empty(), "duplicate names do not join");
  Expect(duplicate_update.reject.has_value(), "duplicate name is rejected");

  dnd::NetClient nameless;
  Expect(nameless.Connect("127.0.0.1", port, ""), "nameless client connects");
  std::vector<dnd::PeerJoined> nameless_joined;
  dnd::NetClientUpdate nameless_update;
  const auto nameless_deadline = Now() + std::chrono::seconds(3);
  while (nameless_joined.empty() && !nameless_update.reject.has_value() &&
         Now() < nameless_deadline) {
    server.Poll(nameless_joined, commands, dropped);
    nameless.Poll(nameless_update);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  Expect(nameless_joined.empty(), "empty player names do not join");
  Expect(nameless_update.reject.has_value(), "empty player name is rejected");
}

auto TestVersionMismatchReject() -> void {
  const std::vector<std::uint8_t> hello =
      dnd::EncodeHello(dnd::Hello{.player_name = "tester"});
  Expect(dnd::DecodeHelloVersion(hello.data(), hello.size()) ==
             dnd::kProtocolVersion,
         "hello header carries the protocol version");
  Expect(!dnd::DecodeHelloVersion(hello.data(), 3).has_value(),
         "truncated hello header is rejected");

  dnd::NetServer server;
  Expect(server.Start(0), "version test server listens");
  const std::uint16_t port = server.Port();
  if (port == 0) {
    return;
  }

  const std::unique_ptr<dnd::TcpConnection> connection =
      dnd::TcpConnection::Connect("127.0.0.1", port);
  Expect(connection != nullptr, "foreign-version client connects");
  if (connection == nullptr) {
    return;
  }

  // Клиент другой версии шлёт обычный Hello, но с чужим номером протокола.
  const std::uint32_t foreign_version = dnd::kProtocolVersion + 1;
  dnd::ByteWriter writer;
  writer.U8(static_cast<std::uint8_t>(dnd::MessageType::kHello));
  writer.U32(foreign_version);
  writer.String("tester");
  writer.String("");
  connection->QueueFrame(writer.Data());

  std::vector<dnd::PeerJoined> joined;
  std::vector<dnd::PeerCommand> commands;
  std::vector<dnd::PeerId> dropped;
  std::vector<std::vector<std::uint8_t>> frames;
  std::optional<std::string> reason;
  const auto deadline = Now() + std::chrono::seconds(3);
  while (!reason.has_value() && Now() < deadline) {
    server.Poll(joined, commands, dropped);
    if (connection->Poll(frames)) {
      for (const std::vector<std::uint8_t>& frame : frames) {
        if (frame.empty() || static_cast<dnd::MessageType>(frame[0]) !=
                                 dnd::MessageType::kReject) {
          continue;
        }
        if (const std::optional<std::string> decoded =
                dnd::DecodeReject(frame.data(), frame.size())) {
          reason = decoded;
        }
      }
      frames.clear();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  Expect(joined.empty(), "foreign version does not join");
  Expect(reason.has_value(), "foreign version is rejected");
  if (reason.has_value()) {
    const std::string host_version = std::to_string(dnd::kProtocolVersion);
    const std::string client_version = std::to_string(foreign_version);
    Expect(reason->find(host_version) != std::string::npos,
           "reject names the host version");
    Expect(reason->find(client_version) != std::string::npos,
           "reject names the client version");
    Expect(reason->find("обновите игру") != std::string::npos,
           "reject tells the player what to do");
  }
}

auto TestPeerLimit() -> void {
  dnd::NetServer server;
  Expect(server.Start(0), "peer limit server listens");
  const std::uint16_t port = server.Port();
  if (port == 0) {
    return;
  }

  std::vector<std::unique_ptr<dnd::NetClient>> clients;
  for (std::size_t i = 0; i < dnd::kMaxPeers; ++i) {
    clients.push_back(std::make_unique<dnd::NetClient>());
    Expect(clients.back()->Connect("127.0.0.1", port,
                                   "player" + std::to_string(i)),
           "seat client connects");
  }

  std::vector<dnd::PeerJoined> joined;
  std::vector<dnd::PeerCommand> commands;
  std::vector<dnd::PeerId> dropped;
  dnd::NetClientUpdate update;
  const auto fill_deadline = Now() + std::chrono::seconds(3);
  while (joined.size() < dnd::kMaxPeers && Now() < fill_deadline) {
    for (const std::unique_ptr<dnd::NetClient>& client : clients) {
      client->Poll(update);
    }
    server.Poll(joined, commands, dropped);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  Expect(joined.size() == dnd::kMaxPeers, "all seats are taken");

  dnd::NetClient extra;
  Expect(extra.Connect("127.0.0.1", port, "extra"), "extra client connects");
  dnd::NetClientUpdate extra_update;
  const auto extra_deadline = Now() + std::chrono::seconds(3);
  while (!extra_update.reject.has_value() && Now() < extra_deadline) {
    for (const std::unique_ptr<dnd::NetClient>& client : clients) {
      client->Poll(update);
    }
    extra.Poll(extra_update);
    server.Poll(joined, commands, dropped);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  Expect(joined.size() == dnd::kMaxPeers, "extra client does not join");
  Expect(extra_update.reject.has_value(), "extra client is rejected");
  if (extra_update.reject.has_value()) {
    Expect(extra_update.reject->find("свободных мест") != std::string::npos,
           "reject explains that the table is full");
  }
}

auto TestCommandRateLimit() -> void {
  dnd::NetServer server;
  Expect(server.Start(0), "rate limit server listens");
  const std::uint16_t port = server.Port();
  if (port == 0) {
    return;
  }

  dnd::NetClient client;
  Expect(client.Connect("127.0.0.1", port, "spammer"), "spammer connects");
  std::vector<dnd::PeerJoined> joined;
  std::vector<dnd::PeerCommand> commands;
  std::vector<dnd::PeerId> dropped;
  dnd::NetClientUpdate update;
  const auto join_deadline = Now() + std::chrono::seconds(3);
  while (joined.empty() && Now() < join_deadline) {
    server.Poll(joined, commands, dropped);
    client.Poll(update);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  Expect(!joined.empty(), "spammer joins");
  if (joined.empty()) {
    return;
  }

  server.SendWelcome(
      joined.front().peer,
      dnd::Welcome{.snapshot = MakeSnapshot(), .fog = MakeFog(6, 4)});
  const auto welcome_deadline = Now() + std::chrono::seconds(3);
  while (!update.welcome.has_value() && Now() < welcome_deadline) {
    server.Poll(joined, commands, dropped);
    client.Poll(update);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  Expect(update.welcome.has_value(), "spammer receives the welcome");

  const int burst = dnd::kMaxCommandsPerSecond * 10;
  for (int i = 0; i < burst; ++i) {
    client.SendCommand(dnd::Say{.text = "flood"});
  }

  // Читаем, пока сервер не замолчит на 200 мс: так виден весь всплеск, но
  // мы не ждём следующего окна лимита.
  const auto drain_deadline = Now() + std::chrono::seconds(3);
  auto quiet_since = Now();
  while (Now() < drain_deadline &&
         Now() - quiet_since < std::chrono::milliseconds(200)) {
    const std::size_t before = commands.size();
    server.Poll(joined, commands, dropped);
    if (commands.size() != before) {
      quiet_since = Now();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  Expect(!commands.empty(), "burst is partially accepted");
  Expect(
      commands.size() <= static_cast<std::size_t>(dnd::kMaxCommandsPerSecond),
      "extra commands are dropped");
}

auto TestOutgoingQueueLimit() -> void {
  dnd::NetServer server;
  Expect(server.Start(0), "queue limit server listens");
  const std::uint16_t port = server.Port();
  if (port == 0) {
    return;
  }

  // Собеседник не читает: хост обязан закрыть такое соединение, а не копить
  // исходящие байты до исчерпания памяти.
  const std::unique_ptr<dnd::TcpConnection> connection =
      dnd::TcpConnection::Connect("127.0.0.1", port);
  Expect(connection != nullptr, "queue limit client connects");
  if (connection == nullptr) {
    return;
  }
  std::vector<std::vector<std::uint8_t>> frames;
  const auto connect_deadline = Now() + std::chrono::seconds(3);
  while (!connection->Connected() && Now() < connect_deadline) {
    connection->Poll(frames);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  Expect(connection->Connected(), "queue limit connection is established");

  const std::vector<std::uint8_t> frame(static_cast<std::size_t>(16) * 1024, 0);
  for (int i = 0; i < 512 && connection->Connected(); ++i) {
    connection->QueueFrame(frame);
  }
  Expect(!connection->Connected(),
         "connection is closed after the outgoing queue limit");
}

auto TestPing() -> void {
  const std::vector<std::uint8_t> ping = dnd::EncodePing();
  Expect(dnd::DecodePing(ping.data(), ping.size()), "ping roundtrip");
  std::vector<std::uint8_t> trailing = ping;
  trailing.push_back(0);
  Expect(!dnd::DecodePing(trailing.data(), trailing.size()),
         "ping rejects trailing bytes");
}

}  // namespace

auto main() -> int {
  try {
    TestPing();
    TestViewUpdateRoundtrip();
    TestPasswordAndRoster();
    TestPlayerNameValidation();
    TestVersionMismatchReject();
    TestPeerLimit();
    TestCommandRateLimit();
    TestOutgoingQueueLimit();
    TestTcpHandshakeAndEvents();
  } catch (const std::exception& error) {
    std::fprintf(stderr, "unexpected exception: %s\n", error.what());
    return 1;
  }

  if (g_failures == 0) {
    std::printf("all network checks passed\n");
    return 0;
  }
  std::fprintf(stderr, "%d network check(s) failed\n", g_failures);
  return 1;
}
