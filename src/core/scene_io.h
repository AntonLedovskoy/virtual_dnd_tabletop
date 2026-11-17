#pragma once

#include <filesystem>
#include <optional>

#include "core/snapshot.h"

namespace dnd {

// Save files are a versioned little-endian binary dump of a GameSnapshot.
inline constexpr auto kSaveVersion = 6U;

auto WriteSnapshot(const GameSnapshot& snapshot,
                   const std::filesystem::path& path) -> bool;
auto ReadSnapshot(const std::filesystem::path& path)
    -> std::optional<GameSnapshot>;

}  // namespace dnd
