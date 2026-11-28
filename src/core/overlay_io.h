#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <vector>

#include "core/overlay.h"

namespace dnd {

// Файл партии — версионированный little-endian дамп Overlay. Снимки внутри
// кодируются форматом сохранения той эпохи: версия 4 — сохранение 33,
// версия 3 — 32, версия 2 — 31, версия 1 — 30. Поэтому смена формата снимка
// поднимает и kOverlayVersion.
inline constexpr auto kOverlayVersion = 6U;
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

// Партия для кампании: её отпечаток и пустой список изменений.
auto FreshOverlay(const Campaign& campaign) -> Overlay;

// Файл партии рядом с кампанией: `<имя>.dndoverlay`.
auto OverlayPath(const std::filesystem::path& campaign_path)
    -> std::filesystem::path;

}  // namespace dnd
