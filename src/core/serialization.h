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

// Какие поля снимка знает версия файла. Появившиеся позже поля в старых
// файлах отсутствуют, декодер подставляет значения по умолчанию.
struct SnapshotFormat {
  bool has_turn_economy = true;    // экономика хода (сохранение 28)
  bool has_damage_profile = true;  // защиты и дистанция атаки (сохранение 29)
  bool has_game_clock = true;  // игровые часы и длинный отдых (сохранение 30)
  bool has_cell_props =
      true;                // свойства клеток и палитра урона (сохранение 31)
  bool has_vision = true;  // свет, затемнение, тёмное зрение (сохранение 32)
  bool has_auto_hp =
      true;  // HP по костям, число костей монстра (сохранение 33)
};

// Формат передачи, общий для сохранений и TCP-сообщений. Декодеры строгие:
// отвергают обрезанные данные, лишние байты и значения вне диапазона.
auto EncodeSnapshot(const GameSnapshot& snapshot) -> std::vector<std::uint8_t>;
// Кодирование в формате прошлой версии: полей, которых тогда не было, в
// файле нет. Нужно тестам совместимости и проверке миграций.
auto EncodeSnapshot(const GameSnapshot& snapshot, const SnapshotFormat& format)
    -> std::vector<std::uint8_t>;
auto DecodeSnapshot(const std::uint8_t* data, std::size_t size,
                    const SnapshotFormat& format = SnapshotFormat{})
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
