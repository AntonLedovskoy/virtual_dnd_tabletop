#include "core/overlay_io.h"

#include <cstdint>
#include <fstream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "core/byte_io.h"
#include "core/campaign_io.h"
#include "core/scene_io.h"
#include "core/serialization.h"

namespace dnd {
namespace {

constexpr std::uint32_t kMagic = 0x4F444E44;  // "DNDO" в little-endian
// Флаги записи: какие поля сцены изменены относительно кампании.
constexpr std::uint8_t kFlagDeleted = 1;
constexpr std::uint8_t kFlagName = 2;
constexpr std::uint8_t kFlagNote = 4;
constexpr std::uint8_t kFlagSnapshot = 8;
constexpr std::uint8_t kKnownFlags =
    kFlagDeleted | kFlagName | kFlagNote | kFlagSnapshot;

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

auto IsOverlayWritable(const Overlay& overlay) -> bool {
  if (overlay.next_scene_id < kOverlaySceneIdBase ||
      overlay.scenes.size() > kMaxOverlayScenes) {
    return false;
  }
  for (std::size_t index = 0; index < overlay.scenes.size(); ++index) {
    const OverlayScene& scene = overlay.scenes[index];
    if (scene.id == 0 || FindOverlayScene(overlay, scene.id) != &scene) {
      return false;
    }
    if (scene.deleted) {
      if (scene.name.has_value() || scene.note.has_value() ||
          scene.snapshot.has_value()) {
        return false;
      }
      continue;
    }
    if (scene.name.has_value() &&
        (scene.name->empty() || scene.name->size() > kMaxSceneNameLength)) {
      return false;
    }
    if (scene.note.has_value() && scene.note->size() > kMaxSceneNoteLength) {
      return false;
    }
    if (scene.id >= kOverlaySceneIdBase && scene.id >= overlay.next_scene_id) {
      return false;
    }
  }
  return true;
}

// Снимки партии версии 1 писались форматом сохранения 30, версии 2 — 31,
// версии 3 — 32, версии 4 — 33.
auto OverlaySnapshotFormat(std::uint32_t overlay_version) -> SnapshotFormat {
  if (overlay_version >= 5) {
    return SnapshotFormat{};
  }
  if (overlay_version >= 4) {
    return SaveSnapshotFormat(33);
  }
  if (overlay_version >= 3) {
    return SaveSnapshotFormat(32);
  }
  if (overlay_version >= 2) {
    return SaveSnapshotFormat(31);
  }
  return SaveSnapshotFormat(30);
}

auto DecodeOverlay(const std::uint8_t* data, std::size_t size,
                   std::uint32_t overlay_version) -> std::optional<Overlay> {
  if (overlay_version < kMinOverlayVersion ||
      overlay_version > kOverlayVersion) {
    return std::nullopt;
  }
  ByteReader reader(data, size);
  Overlay overlay;
  std::uint32_t scene_count = 0;
  if (!reader.U64(overlay.campaign_fingerprint) ||
      !reader.U64(overlay.active) || !reader.U64(overlay.next_scene_id) ||
      !reader.U32(scene_count)) {
    return std::nullopt;
  }
  if (overlay.next_scene_id < kOverlaySceneIdBase ||
      scene_count > kMaxOverlayScenes) {
    return std::nullopt;
  }

  for (std::uint32_t index = 0; index < scene_count; ++index) {
    std::uint64_t raw_id = 0;
    std::uint8_t flags = 0;
    if (!reader.U64(raw_id) || !reader.U8(flags)) {
      return std::nullopt;
    }
    if (raw_id == 0 || (flags & ~kKnownFlags) != 0 ||
        ((flags & kFlagDeleted) != 0 && (flags & ~kFlagDeleted) != 0)) {
      return std::nullopt;
    }
    OverlayScene scene;
    scene.id = raw_id;
    scene.deleted = (flags & kFlagDeleted) != 0;
    if ((flags & kFlagName) != 0) {
      std::string name;
      if (!reader.String(name, kMaxSceneNameLength) || name.empty()) {
        return std::nullopt;
      }
      scene.name = std::move(name);
    }
    if ((flags & kFlagNote) != 0) {
      std::string note;
      if (!reader.String(note, kMaxSceneNoteLength)) {
        return std::nullopt;
      }
      scene.note = std::move(note);
    }
    if ((flags & kFlagSnapshot) != 0) {
      std::uint32_t blob_size = 0;
      std::vector<std::uint8_t> blob;
      if (!reader.U32(blob_size) || blob_size == 0 ||
          !reader.Bytes(blob_size, blob)) {
        return std::nullopt;
      }
      const std::optional<GameSnapshot> snapshot = DecodeSnapshot(
          blob.data(), blob.size(), OverlaySnapshotFormat(overlay_version));
      if (!snapshot.has_value()) {
        return std::nullopt;
      }
      scene.snapshot = *snapshot;
    }
    if (FindOverlayScene(overlay, scene.id) != nullptr ||
        overlay.scenes.size() >= kMaxOverlayScenes) {
      return std::nullopt;
    }
    if (scene.id >= kOverlaySceneIdBase && scene.id >= overlay.next_scene_id) {
      return std::nullopt;
    }
    overlay.scenes.push_back(std::move(scene));
  }
  if (!reader.Done()) {
    return std::nullopt;
  }
  return overlay;
}

}  // namespace

auto WriteOverlay(const Overlay& overlay, const std::filesystem::path& path)
    -> bool {
  if (!IsOverlayWritable(overlay)) {
    return false;
  }
  const std::vector<std::uint8_t> payload =
      EncodeOverlayAs(overlay, kOverlayVersion);
  if (payload.empty() || payload.size() > kMaxOverlayBytes) {
    return false;
  }
  std::filesystem::path temp = path;
  temp += ".tmp";
  {
    std::ofstream out(temp, std::ios::binary | std::ios::trunc);
    if (!out) {
      return false;
    }
    WriteU32(out, kMagic);
    WriteU32(out, kOverlayVersion);
    WriteU32(out, static_cast<std::uint32_t>(payload.size()));
    out.write(reinterpret_cast<const char*>(payload.data()),
              static_cast<std::streamsize>(payload.size()));
    out.flush();
    if (!out.good()) {
      out.close();
      std::error_code ignored;
      std::filesystem::remove(temp, ignored);
      return false;
    }
  }
  std::error_code error;
  std::filesystem::rename(temp, path, error);
  if (error) {
    std::error_code ignored;
    std::filesystem::remove(temp, ignored);
    return false;
  }
  return true;
}

auto ReadOverlay(const std::filesystem::path& path) -> std::optional<Overlay> {
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
  if (!ReadU32(in, version) || version < kMinOverlayVersion ||
      version > kOverlayVersion) {
    return std::nullopt;
  }
  if (!ReadU32(in, payload_size) || payload_size == 0 ||
      payload_size > kMaxOverlayBytes) {
    return std::nullopt;
  }
  std::vector<std::uint8_t> payload(payload_size);
  in.read(reinterpret_cast<char*>(payload.data()),
          static_cast<std::streamsize>(payload_size));
  if (!in) {
    return std::nullopt;
  }
  return DecodeOverlay(payload.data(), payload.size(), version);
}

auto EncodeOverlayAs(const Overlay& overlay, std::uint32_t overlay_version)
    -> std::vector<std::uint8_t> {
  if (overlay_version < kMinOverlayVersion ||
      overlay_version > kOverlayVersion) {
    return {};
  }
  ByteWriter writer;
  writer.U64(overlay.campaign_fingerprint);
  writer.U64(overlay.active);
  writer.U64(overlay.next_scene_id);
  writer.U32(static_cast<std::uint32_t>(overlay.scenes.size()));
  for (const OverlayScene& scene : overlay.scenes) {
    std::uint8_t flags = 0;
    if (scene.deleted) {
      flags |= kFlagDeleted;
    }
    if (scene.name.has_value()) {
      flags |= kFlagName;
    }
    if (scene.note.has_value()) {
      flags |= kFlagNote;
    }
    if (scene.snapshot.has_value()) {
      flags |= kFlagSnapshot;
    }
    writer.U64(scene.id);
    writer.U8(flags);
    if (scene.name.has_value()) {
      writer.String(*scene.name);
    }
    if (scene.note.has_value()) {
      writer.String(*scene.note);
    }
    if (scene.snapshot.has_value()) {
      const std::vector<std::uint8_t> blob = EncodeSnapshot(
          *scene.snapshot, OverlaySnapshotFormat(overlay_version));
      writer.U32(static_cast<std::uint32_t>(blob.size()));
      writer.Raw(blob);
    }
  }
  return writer.Data();
}

auto CampaignFingerprint(const Campaign& campaign) -> std::uint64_t {
  const std::vector<std::uint8_t> bytes =
      EncodeCampaignAs(campaign, kCampaignVersion);
  std::uint64_t hash = 14695981039346656037ULL;
  for (const std::uint8_t byte : bytes) {
    hash ^= byte;
    hash *= 1099511628211ULL;
  }
  return hash;
}

auto FreshOverlay(const Campaign& campaign) -> Overlay {
  return Overlay{.campaign_fingerprint = CampaignFingerprint(campaign),
                 .active = 0,
                 .next_scene_id = kOverlaySceneIdBase};
}

auto OverlayPath(const std::filesystem::path& campaign_path)
    -> std::filesystem::path {
  std::filesystem::path path = campaign_path;
  path.replace_extension(".dndoverlay");
  return path;
}

}  // namespace dnd
