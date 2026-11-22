#pragma once

#include <filesystem>
#include <optional>

#include "core/snapshot.h"

namespace dnd {

// Файлы сохранения — версионированный little-endian дамп GameSnapshot.
inline constexpr auto kSaveVersion = 30U;

auto WriteSnapshot(const GameSnapshot& snapshot,
                   const std::filesystem::path& path) -> bool;
auto ReadSnapshot(const std::filesystem::path& path)
    -> std::optional<GameSnapshot>;

}  // namespace dnd
