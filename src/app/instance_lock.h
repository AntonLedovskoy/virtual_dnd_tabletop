#pragma once

#include <filesystem>

namespace dnd {

// Именованный мьютекс на папку игры: два экземпляра из одной папки ломают
// подмену exe (`virtual_dnd_tabletop.old.exe` остаётся залоченным). false —
// экземпляр из этой папки уже запущен.
auto AcquireInstanceLock(const std::filesystem::path& base_path) -> bool;

// Отпускает мьютекс перед запуском обновлённого exe: новый процесс должен
// получить его, не дожидаясь выхода старого.
auto ReleaseInstanceLock() -> void;

}  // namespace dnd
