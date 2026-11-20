#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>

#include "core/campaign.h"

namespace dnd {

// Файл кампании — версионированный little-endian дамп всех сцен. Формат
// сцены совпадает с отдельным файлом сохранения.
inline constexpr auto kCampaignVersion = 6U;

// Сколько сцен допускает файл кампании.
inline constexpr std::size_t kMaxCampaignScenes = 64;

auto WriteCampaign(const Campaign& campaign, const std::filesystem::path& path)
    -> bool;
auto ReadCampaign(const std::filesystem::path& path) -> std::optional<Campaign>;

// Имена файлов *.dndcamp в каталоге, по алфавиту. Ошибка чтения каталога
// даёт пустой список.
auto ListCampaignFiles(const std::filesystem::path& directory)
    -> std::vector<std::string>;

}  // namespace dnd
