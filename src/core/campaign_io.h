#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "core/campaign.h"

namespace dnd {

// Файл кампании — версионированный little-endian дамп всех сцен. Формат
// сцены совпадает с отдельным файлом сохранения.
// 17: у сцен появились стабильные id, у кампании — счётчик next_scene_id.
inline constexpr auto kCampaignVersion = 20U;
// Самая старая читаемая версия — кампании v0.1.0 (13); их сцены хранят
// снимки сохранения 26.
inline constexpr auto kMinCampaignVersion = 13U;

// Сколько сцен допускает файл кампании.
inline constexpr std::size_t kMaxCampaignScenes = 64;

auto WriteCampaign(const Campaign& campaign, const std::filesystem::path& path)
    -> bool;
auto ReadCampaign(const std::filesystem::path& path) -> std::optional<Campaign>;

// Кодирует кампанию в формате указанной версии — для тестов совместимости.
auto EncodeCampaignAs(const Campaign& campaign, std::uint32_t campaign_version)
    -> std::vector<std::uint8_t>;

// Имена файлов *.dndcamp в каталоге, по алфавиту. Ошибка чтения каталога
// даёт пустой список.
auto ListCampaignFiles(const std::filesystem::path& directory)
    -> std::vector<std::string>;

}  // namespace dnd
