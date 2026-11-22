#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <vector>

#include "core/overlay.h"

namespace dnd {

// Файл партии — версионированный little-endian дамп Overlay. Снимки внутри
// кодируются текущим форматом сохранения, поэтому kSaveVersion и протокол от
// появления оверлея не меняются.
inline constexpr auto kOverlayVersion = 1U;
inline constexpr auto kMinOverlayVersion = 1U;
// Предел payload: снимков до kMaxOverlayScenes сцен.
inline constexpr std::size_t kMaxOverlayBytes =
    static_cast<std::size_t>(256) * 1024U * 1024U;

// Запись атомарная: временный файл рядом и rename поверх старого. Обрыв на
// записи не должен стоить партии.
auto WriteOverlay(const Overlay& overlay, const std::filesystem::path& path)
    -> bool;
// Повреждённый или чужой файл — nullopt; вызывающий не затирает его молча.
auto ReadOverlay(const std::filesystem::path& path) -> std::optional<Overlay>;

// Кодирует партию в формате указанной версии — для тестов совместимости.
auto EncodeOverlayAs(const Overlay& overlay, std::uint32_t overlay_version)
    -> std::vector<std::uint8_t>;

// Отпечаток содержимого кампании (FNV-1a по канонической записи): партия
// применяется к изменившейся кампании только после подтверждения DM.
auto CampaignFingerprint(const Campaign& campaign) -> std::uint64_t;

}  // namespace dnd
