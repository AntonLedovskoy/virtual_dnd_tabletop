#include "core/campaign_io.h"

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "core/byte_io.h"
#include "core/conditions.h"
#include "core/serialization.h"

namespace dnd {
namespace {

constexpr std::uint32_t kMagic = 0x43444E44;  // "DNDC" в little-endian
constexpr std::uint32_t kMaxPayload = 256U * 1024U * 1024U;
// До версии 13 у состояний не было КС; до версии 12 длительность была в ходах.
// Версия 14 не знает полей экономики хода (действие, реакция, рывок) и защит
// от урона, версия 15 — игровых часов.
constexpr std::uint32_t kLegacyRoundsCampaignVersion = 11;
constexpr std::uint32_t kLegacySecondsCampaignVersion = 12;
constexpr std::uint32_t kPreviousCampaignVersion = 15;

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

auto EncodeCampaign(const Campaign& campaign) -> std::vector<std::uint8_t> {
  ByteWriter writer;
  writer.U32(static_cast<std::uint32_t>(campaign.scenes.size()));
  writer.U32(static_cast<std::uint32_t>(campaign.active));
  for (const Scene& scene : campaign.scenes) {
    writer.String(scene.name);
    writer.String(scene.note);
    const std::vector<std::uint8_t> blob = EncodeSnapshot(scene.snapshot);
    writer.U32(static_cast<std::uint32_t>(blob.size()));
    writer.Raw(blob);
  }
  return writer.Data();
}

auto DecodeCampaign(const std::uint8_t* data, std::size_t size,
                    bool conditions_have_dc, bool has_turn_economy,
                    bool has_damage_profile, bool has_game_clock)
    -> std::optional<Campaign> {
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
  campaign.scenes.reserve(scene_count);
  for (std::uint32_t i = 0; i < scene_count; ++i) {
    Scene scene;
    std::uint32_t blob_size = 0;
    std::vector<std::uint8_t> blob;
    if (!reader.String(scene.name, kMaxSceneNameLength) || scene.name.empty() ||
        !reader.String(scene.note, kMaxSceneNoteLength) ||
        !reader.U32(blob_size) || blob_size == 0 ||
        !reader.Bytes(blob_size, blob)) {
      return std::nullopt;
    }
    const std::optional<GameSnapshot> snapshot =
        DecodeSnapshot(blob.data(), blob.size(), conditions_have_dc,
                       has_turn_economy, has_damage_profile, has_game_clock);
    if (!snapshot.has_value()) {
      return std::nullopt;
    }
    scene.snapshot = *snapshot;
    campaign.scenes.push_back(std::move(scene));
  }
  if (!reader.Done()) {
    return std::nullopt;
  }
  campaign.active = active;
  return campaign;
}

}  // namespace

auto WriteCampaign(const Campaign& campaign, const std::filesystem::path& path)
    -> bool {
  if (campaign.scenes.empty() || campaign.active >= campaign.scenes.size()) {
    return false;
  }
  const std::vector<std::uint8_t> payload = EncodeCampaign(campaign);
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
  if (!ReadU32(in, version) ||
      (version != kCampaignVersion && version != kPreviousCampaignVersion &&
       version != kLegacySecondsCampaignVersion &&
       version != kLegacyRoundsCampaignVersion)) {
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
  std::optional<Campaign> campaign = DecodeCampaign(
      payload.data(), payload.size(),
      version == kCampaignVersion || version == kPreviousCampaignVersion,
      version >= kPreviousCampaignVersion, version >= kPreviousCampaignVersion,
      version >= kCampaignVersion);
  if (campaign.has_value() && version == kLegacyRoundsCampaignVersion) {
    for (Scene& scene : campaign->scenes) {
      for (Token& token : scene.snapshot.tokens) {
        ConvertLegacyConditionRounds(token.conditions);
      }
    }
  }
  return campaign;
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
