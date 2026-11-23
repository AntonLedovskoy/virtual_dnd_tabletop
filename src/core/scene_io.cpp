#include "core/scene_io.h"

#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

#include "core/byte_io.h"
#include "core/serialization.h"

namespace dnd {
namespace {

constexpr std::uint32_t kMagic = 0x53444E44;  // "DNDS" в little-endian
constexpr std::uint32_t kMaxPayload = 64U * 1024U * 1024U;
// Что появилось когда: экономика хода — сохранение 28, защиты от урона и
// дистанция атаки — 29, игровые часы и время длинного отдыха — 30,
// свойства клеток и палитра урона — 31.
constexpr std::uint32_t kTurnEconomySaveVersion = 28;
constexpr std::uint32_t kDamageProfileSaveVersion = 29;
constexpr std::uint32_t kGameClockSaveVersion = 30;
constexpr std::uint32_t kCellPropsSaveVersion = 31;

auto ReadU32(std::istream& in, std::uint32_t& value) -> bool {
  std::uint32_t result = 0;
  for (int shift = 0; shift < 32; shift += 8) {
    const int byte = in.get();
    if (byte < 0) {
      return false;
    }
    result |= static_cast<std::uint32_t>(static_cast<unsigned char>(byte))
              << shift;
  }
  value = result;
  return true;
}

auto WriteU32(std::ostream& out, std::uint32_t value) -> void {
  for (int shift = 0; shift < 32; shift += 8) {
    out.put(static_cast<char>((value >> shift) & 0xFFU));
  }
}

}  // namespace

auto SaveSnapshotFormat(std::uint32_t save_version) -> SnapshotFormat {
  return SnapshotFormat{
      .has_turn_economy = save_version >= kTurnEconomySaveVersion,
      .has_damage_profile = save_version >= kDamageProfileSaveVersion,
      .has_game_clock = save_version >= kGameClockSaveVersion,
      .has_cell_props = save_version >= kCellPropsSaveVersion,
  };
}

auto WriteSnapshot(const GameSnapshot& snapshot,
                   const std::filesystem::path& path) -> bool {
  const std::vector<std::uint8_t> payload = EncodeSnapshot(snapshot);
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) {
    return false;
  }
  WriteU32(out, kMagic);
  WriteU32(out, kSaveVersion);
  WriteU32(out, static_cast<std::uint32_t>(payload.size()));
  out.write(reinterpret_cast<const char*>(payload.data()),
            static_cast<std::streamsize>(payload.size()));
  out.flush();
  return out.good();
}

auto ReadSnapshot(const std::filesystem::path& path)
    -> std::optional<GameSnapshot> {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return std::nullopt;
  }

  std::uint32_t magic = 0;
  std::uint32_t version = 0;
  std::uint32_t payload_size = 0;
  if (!ReadU32(in, magic) || magic != kMagic) {
    return std::nullopt;
  }
  if (!ReadU32(in, version) || version < kMinSaveVersion ||
      version > kSaveVersion) {
    return std::nullopt;
  }
  if (!ReadU32(in, payload_size) || payload_size == 0 ||
      payload_size > kMaxPayload) {
    return std::nullopt;
  }

  std::vector<std::uint8_t> payload(payload_size);
  in.read(reinterpret_cast<char*>(payload.data()),
          static_cast<std::streamsize>(payload_size));
  if (!in) {
    return std::nullopt;
  }
  return DecodeSnapshot(payload.data(), payload.size(),
                        SaveSnapshotFormat(version));
}

auto EncodeSnapshotAs(const GameSnapshot& snapshot, std::uint32_t save_version)
    -> std::vector<std::uint8_t> {
  return EncodeSnapshot(snapshot, SaveSnapshotFormat(save_version));
}

}  // namespace dnd
