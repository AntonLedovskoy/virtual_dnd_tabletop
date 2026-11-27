#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <vector>

#include "core/serialization.h"
#include "core/snapshot.h"

namespace dnd {

// Файлы сохранения — версионированный little-endian дамп GameSnapshot.
inline constexpr auto kSaveVersion = 34U;
// Самая старая читаемая версия — сохранения v0.1.0 (26). Раньше в токене не
// было запаса костей хитов, а формат сохранения 25 и старше не восстановить.
inline constexpr auto kMinSaveVersion = 26U;

// Черты снимка, которые знает версия сохранения: новые поля по версии.
auto SaveSnapshotFormat(std::uint32_t save_version) -> SnapshotFormat;

auto WriteSnapshot(const GameSnapshot& snapshot,
                   const std::filesystem::path& path) -> bool;
auto ReadSnapshot(const std::filesystem::path& path)
    -> std::optional<GameSnapshot>;

// Кодирует снимок в формате указанной версии — для тестов совместимости.
auto EncodeSnapshotAs(const GameSnapshot& snapshot, std::uint32_t save_version)
    -> std::vector<std::uint8_t>;

}  // namespace dnd
