#include "core/campaign_io.h"

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "core/byte_io.h"
#include "core/scene_io.h"
#include "core/serialization.h"

namespace dnd {
namespace {

constexpr std::uint32_t kMagic = 0x43444E44;  // "DNDC" в little-endian
constexpr std::uint32_t kMaxPayload = 256U * 1024U * 1024U;

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

// Кампания 13 хранит снимки сохранения 26, 14 — 27, 15 — 28 или 29, 16–17 —
// 30, 18 — текущий снимок. Набор полей берём из версии сохранения, чтобы
// история формата жила в одном месте (scene_io).
auto CampaignSnapshotFormat(std::uint32_t campaign_version) -> SnapshotFormat {
  if (campaign_version >= 18) {
    return SaveSnapshotFormat(kSaveVersion);
  }
  if (campaign_version >= 16) {
    return SaveSnapshotFormat(30);
  }
  if (campaign_version >= 15) {
    // Версия 28 не знает защит от урона; такой файл ловится повтором в
    // ReadCampaign, поэтому по умолчанию считаем снимки версией 29.
    return SaveSnapshotFormat(29);
  }
  return SaveSnapshotFormat(27);
}

// Сцены и счётчик id пишутся начиная с версии 17; в старых файлах id
// выводятся из порядка сцен.
constexpr std::uint32_t kSceneIdCampaignVersion = 17;

auto EncodeCampaign(const Campaign& campaign, std::uint32_t campaign_version,
                    const SnapshotFormat& format) -> std::vector<std::uint8_t> {
  ByteWriter writer;
  writer.U32(static_cast<std::uint32_t>(campaign.scenes.size()));
  writer.U32(static_cast<std::uint32_t>(campaign.active));
  if (campaign_version >= kSceneIdCampaignVersion) {
    writer.U64(campaign.next_scene_id);
  }
  for (const Scene& scene : campaign.scenes) {
    if (campaign_version >= kSceneIdCampaignVersion) {
      writer.U64(scene.id);
    }
    writer.String(scene.name);
    writer.String(scene.note);
    const std::vector<std::uint8_t> blob =
        EncodeSnapshot(scene.snapshot, format);
    writer.U32(static_cast<std::uint32_t>(blob.size()));
    writer.Raw(blob);
  }
  return writer.Data();
}

auto DecodeCampaign(const std::uint8_t* data, std::size_t size,
                    std::uint32_t campaign_version,
                    const SnapshotFormat& format) -> std::optional<Campaign> {
  ByteReader reader(data, size);
  std::uint32_t scene_count = 0;
  std::uint32_t active = 0;
  if (!reader.U32(scene_count) || !reader.U32(active)) {
    return std::nullopt;
  }
  if (scene_count == 0 || scene_count > kMaxCampaignScenes ||
      active >= scene_count) {
    return std::nullopt;
  }

  Campaign campaign;
  if (campaign_version >= kSceneIdCampaignVersion &&
      !reader.U64(campaign.next_scene_id)) {
    return std::nullopt;
  }
  campaign.scenes.reserve(scene_count);
  for (std::uint32_t i = 0; i < scene_count; ++i) {
    Scene scene;
    if (campaign_version >= kSceneIdCampaignVersion) {
      if (!reader.U64(scene.id) || scene.id == 0 ||
          scene.id >= kOverlaySceneIdBase ||
          FindSceneIndex(campaign, scene.id) != kNoScene) {
        return std::nullopt;
      }
    } else {
      scene.id = static_cast<SceneId>(i) + 1;
    }
    std::uint32_t blob_size = 0;
    std::vector<std::uint8_t> blob;
    if (!reader.String(scene.name, kMaxSceneNameLength) || scene.name.empty() ||
        !reader.String(scene.note, kMaxSceneNoteLength) ||
        !reader.U32(blob_size) || blob_size == 0 ||
        !reader.Bytes(blob_size, blob)) {
      return std::nullopt;
    }
    const std::optional<GameSnapshot> snapshot =
        DecodeSnapshot(blob.data(), blob.size(), format);
    if (!snapshot.has_value()) {
      return std::nullopt;
    }
    scene.snapshot = *snapshot;
    campaign.scenes.push_back(std::move(scene));
  }
  if (!reader.Done()) {
    return std::nullopt;
  }
  if (campaign_version < kSceneIdCampaignVersion) {
    campaign.next_scene_id = static_cast<SceneId>(scene_count) + 1;
  }
  if (campaign.next_scene_id == 0 ||
      campaign.next_scene_id >= kOverlaySceneIdBase ||
      FindSceneIndex(campaign, campaign.next_scene_id) != kNoScene) {
    return std::nullopt;
  }
  for (const Scene& scene : campaign.scenes) {
    if (scene.id >= campaign.next_scene_id) {
      return std::nullopt;
    }
  }
  campaign.active = active;
  return campaign;
}

// Кампанию пишет только редактор: id должны быть настоящими, без сцен
// партии и без повторов.
auto IsCampaignWritable(const Campaign& campaign) -> bool {
  if (campaign.scenes.empty() || campaign.active >= campaign.scenes.size()) {
    return false;
  }
  if (campaign.next_scene_id == 0 ||
      campaign.next_scene_id >= kOverlaySceneIdBase) {
    return false;
  }
  for (std::size_t index = 0; index < campaign.scenes.size(); ++index) {
    const Scene& scene = campaign.scenes[index];
    if (scene.id == 0 || scene.id >= kOverlaySceneIdBase ||
        scene.overlay_only || FindSceneIndex(campaign, scene.id) != index ||
        scene.id >= campaign.next_scene_id) {
      return false;
    }
  }
  return true;
}

}  // namespace

auto WriteCampaign(const Campaign& campaign, const std::filesystem::path& path)
    -> bool {
  if (!IsCampaignWritable(campaign)) {
    return false;
  }
  const std::vector<std::uint8_t> payload =
      EncodeCampaign(campaign, kCampaignVersion, SnapshotFormat{});
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) {
    return false;
  }
  WriteU32(out, kMagic);
  WriteU32(out, kCampaignVersion);
  WriteU32(out, static_cast<std::uint32_t>(payload.size()));
  out.write(reinterpret_cast<const char*>(payload.data()),
            static_cast<std::streamsize>(payload.size()));
  out.flush();
  return out.good();
}

auto ReadCampaign(const std::filesystem::path& path)
    -> std::optional<Campaign> {
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
  if (!ReadU32(in, version) || version < kMinCampaignVersion ||
      version > kCampaignVersion) {
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
  SnapshotFormat format = CampaignSnapshotFormat(version);
  std::optional<Campaign> campaign =
      DecodeCampaign(payload.data(), payload.size(), version, format);
  // Кампания 15 писалась и до появления защит от урона (снимки 28), поэтому
  // пробуем и старый набор полей: строгий декодер не примет лишние байты.
  if (!campaign.has_value() && version == 15) {
    format.has_damage_profile = false;
    campaign = DecodeCampaign(payload.data(), payload.size(), version, format);
  }
  return campaign;
}

auto EncodeCampaignAs(const Campaign& campaign, std::uint32_t campaign_version)
    -> std::vector<std::uint8_t> {
  return EncodeCampaign(campaign, campaign_version,
                        CampaignSnapshotFormat(campaign_version));
}

auto ListCampaignFiles(const std::filesystem::path& directory)
    -> std::vector<std::string> {
  std::vector<std::string> files;
  std::error_code error;
  if (directory.empty()) {
    return files;
  }
  for (const auto& entry :
       std::filesystem::directory_iterator(directory, error)) {
    if (error || entry.path().extension() != ".dndcamp") {
      continue;
    }
    if (!entry.is_regular_file(error) || error) {
      error.clear();
      continue;
    }
    files.push_back(entry.path().filename().string());
  }
  std::ranges::sort(files);
  return files;
}

}  // namespace dnd
