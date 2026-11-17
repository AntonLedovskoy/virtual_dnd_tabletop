#include "net/protocol.h"

#include <algorithm>
#include <utility>

#include "core/byte_io.h"
#include "core/serialization.h"

namespace dnd {
namespace {

constexpr std::uint32_t kMaxPlayerName = 64;
constexpr std::uint32_t kMaxPassword = 128;
constexpr std::uint32_t kMaxRejectReason = 256;
constexpr std::uint32_t kMaxRosterEntries = 64;

}  // namespace

auto EncodeHello(const Hello& hello) -> std::vector<std::uint8_t> {
  ByteWriter writer;
  writer.U8(static_cast<std::uint8_t>(MessageType::kHello));
  writer.U32(kProtocolVersion);
  writer.String(hello.player_name);
  writer.String(hello.password);
  return writer.Data();
}

auto DecodeHello(const std::uint8_t* data, std::size_t size)
    -> std::optional<Hello> {
  ByteReader reader(data, size);
  std::uint8_t type = 0;
  std::uint32_t version = 0;
  Hello hello;
  if (!reader.U8(type) ||
      type != static_cast<std::uint8_t>(MessageType::kHello)) {
    return std::nullopt;
  }
  if (!reader.U32(version) || version != kProtocolVersion ||
      !reader.String(hello.player_name, kMaxPlayerName) ||
      !reader.String(hello.password, kMaxPassword) || !reader.Done()) {
    return std::nullopt;
  }
  return hello;
}

auto EncodeWelcome(const Welcome& welcome) -> std::vector<std::uint8_t> {
  const std::vector<std::uint8_t> snapshot = EncodeSnapshot(welcome.snapshot);
  const std::vector<std::uint8_t> fog = EncodeFog(welcome.fog);
  ByteWriter writer;
  writer.U8(static_cast<std::uint8_t>(MessageType::kWelcome));
  writer.U32(static_cast<std::uint32_t>(snapshot.size()));
  writer.Raw(snapshot);
  writer.U32(static_cast<std::uint32_t>(fog.size()));
  writer.Raw(fog);
  writer.U8(welcome.is_dm ? 1 : 0);
  return writer.Data();
}

auto DecodeWelcome(const std::uint8_t* data, std::size_t size)
    -> std::optional<Welcome> {
  ByteReader reader(data, size);
  std::uint8_t type = 0;
  if (!reader.U8(type) ||
      type != static_cast<std::uint8_t>(MessageType::kWelcome)) {
    return std::nullopt;
  }

  std::uint32_t snapshot_size = 0;
  std::vector<std::uint8_t> snapshot_bytes;
  if (!reader.U32(snapshot_size) ||
      !reader.Bytes(snapshot_size, snapshot_bytes)) {
    return std::nullopt;
  }
  std::optional<GameSnapshot> snapshot =
      DecodeSnapshot(snapshot_bytes.data(), snapshot_bytes.size());
  if (!snapshot.has_value()) {
    return std::nullopt;
  }

  std::uint32_t fog_size = 0;
  std::vector<std::uint8_t> fog_bytes;
  if (!reader.U32(fog_size) || !reader.Bytes(fog_size, fog_bytes)) {
    return std::nullopt;
  }
  std::optional<FogSnapshot> fog =
      DecodeFog(fog_bytes.data(), fog_bytes.size());
  if (!fog.has_value()) {
    return std::nullopt;
  }

  std::uint8_t is_dm = 0;
  if (!reader.U8(is_dm) || !reader.Done()) {
    return std::nullopt;
  }

  return Welcome{
      .snapshot = std::move(*snapshot),
      .fog = std::move(*fog),
      .is_dm = is_dm != 0,
  };
}

auto EncodeCommandMessage(const Command& command) -> std::vector<std::uint8_t> {
  ByteWriter writer;
  writer.U8(static_cast<std::uint8_t>(MessageType::kCommand));
  writer.Raw(EncodeCommand(command));
  return writer.Data();
}

auto DecodeCommandMessage(const std::uint8_t* data, std::size_t size)
    -> std::optional<Command> {
  ByteReader reader(data, size);
  std::uint8_t type = 0;
  if (!reader.U8(type) ||
      type != static_cast<std::uint8_t>(MessageType::kCommand)) {
    return std::nullopt;
  }
  return DecodeCommand(reader.Data(), reader.Remaining());
}

auto EncodeEventMessage(const Event& event) -> std::vector<std::uint8_t> {
  ByteWriter writer;
  writer.U8(static_cast<std::uint8_t>(MessageType::kEvent));
  writer.Raw(EncodeEvent(event));
  return writer.Data();
}

auto DecodeEventMessage(const std::uint8_t* data, std::size_t size)
    -> std::optional<Event> {
  ByteReader reader(data, size);
  std::uint8_t type = 0;
  if (!reader.U8(type) ||
      type != static_cast<std::uint8_t>(MessageType::kEvent)) {
    return std::nullopt;
  }
  return DecodeEvent(reader.Data(), reader.Remaining());
}

auto EncodeReject(const std::string& reason) -> std::vector<std::uint8_t> {
  ByteWriter writer;
  writer.U8(static_cast<std::uint8_t>(MessageType::kReject));
  writer.String(reason.size() > kMaxRejectReason
                    ? reason.substr(0, kMaxRejectReason)
                    : reason);
  return writer.Data();
}

auto DecodeReject(const std::uint8_t* data, std::size_t size)
    -> std::optional<std::string> {
  ByteReader reader(data, size);
  std::uint8_t type = 0;
  std::string reason;
  if (!reader.U8(type) ||
      type != static_cast<std::uint8_t>(MessageType::kReject) ||
      !reader.String(reason, kMaxRejectReason) || !reader.Done()) {
    return std::nullopt;
  }
  return reason;
}

auto EncodeRoster(const std::vector<RosterEntry>& roster)
    -> std::vector<std::uint8_t> {
  ByteWriter writer;
  writer.U8(static_cast<std::uint8_t>(MessageType::kRoster));
  const std::size_t count =
      std::min<std::size_t>(roster.size(), kMaxRosterEntries);
  writer.U32(static_cast<std::uint32_t>(count));
  for (std::size_t i = 0; i < count; ++i) {
    const RosterEntry& entry = roster[i];
    writer.String(entry.name);
    writer.U8(entry.is_dm ? 1 : 0);
  }
  return writer.Data();
}

auto DecodeRoster(const std::uint8_t* data, std::size_t size)
    -> std::optional<std::vector<RosterEntry>> {
  ByteReader reader(data, size);
  std::uint8_t type = 0;
  std::uint32_t count = 0;
  if (!reader.U8(type) ||
      type != static_cast<std::uint8_t>(MessageType::kRoster) ||
      !reader.U32(count) || count > kMaxRosterEntries) {
    return std::nullopt;
  }
  std::vector<RosterEntry> roster;
  roster.reserve(count);
  for (std::uint32_t i = 0; i < count; ++i) {
    RosterEntry entry;
    std::uint8_t is_dm = 0;
    if (!reader.String(entry.name, kMaxPlayerName) || !reader.U8(is_dm)) {
      return std::nullopt;
    }
    entry.is_dm = is_dm != 0;
    roster.push_back(std::move(entry));
  }
  if (!reader.Done()) {
    return std::nullopt;
  }
  return roster;
}

auto EncodePing() -> std::vector<std::uint8_t> {
  ByteWriter writer;
  writer.U8(static_cast<std::uint8_t>(MessageType::kPing));
  return writer.Data();
}

auto DecodePing(const std::uint8_t* data, std::size_t size) -> bool {
  ByteReader reader(data, size);
  std::uint8_t type = 0;
  return reader.U8(type) &&
         type == static_cast<std::uint8_t>(MessageType::kPing) && reader.Done();
}

}  // namespace dnd
