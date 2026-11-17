#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "core/command.h"
#include "core/event.h"
#include "core/snapshot.h"
#include "core/visibility.h"

namespace dnd {

// Wire format shared by save files and TCP messages. Decoders are strict:
// they reject truncated input, trailing bytes and out-of-range values.
auto EncodeSnapshot(const GameSnapshot& snapshot) -> std::vector<std::uint8_t>;
auto DecodeSnapshot(const std::uint8_t* data, std::size_t size)
    -> std::optional<GameSnapshot>;

auto EncodeFog(const FogSnapshot& fog) -> std::vector<std::uint8_t>;
auto DecodeFog(const std::uint8_t* data, std::size_t size)
    -> std::optional<FogSnapshot>;

auto EncodeCommand(const Command& command) -> std::vector<std::uint8_t>;
auto DecodeCommand(const std::uint8_t* data, std::size_t size)
    -> std::optional<Command>;

auto EncodeEvent(const Event& event) -> std::vector<std::uint8_t>;
auto DecodeEvent(const std::uint8_t* data, std::size_t size)
    -> std::optional<Event>;

}  // namespace dnd
