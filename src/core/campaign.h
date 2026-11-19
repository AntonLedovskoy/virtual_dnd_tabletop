#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/snapshot.h"

namespace dnd {

inline constexpr std::size_t kMaxSceneNameLength = 64;
inline constexpr std::size_t kMaxSceneNoteLength = 512;
// Значение ignore_index, когда проверяется имя новой сцены.
inline constexpr std::size_t kNoScene = static_cast<std::size_t>(-1);

// Одна сцена кампании: имя, заметка DM и полное авторитетное состояние мира.
struct Scene {
  std::string name;
  std::string note;
  GameSnapshot snapshot;
};

// Кампания — набор сцен, между которыми DM переключает стол. Активная сцена
// существует всегда: scenes не пуст, а active указывает внутрь него.
struct Campaign {
  std::vector<Scene> scenes;
  std::size_t active = 0;
};

// Кампания из одной сцены; имя пустое заменяется на «Сцена 1».
auto MakeCampaign(GameSnapshot snapshot, std::string name) -> Campaign;

// Свободное имя для новой сцены: «Сцена 1», «Сцена 2», ...
auto NextSceneName(const Campaign& campaign) -> std::string;

// Имя непустое, не длиннее kMaxSceneNameLength и не занято другой сценой.
[[nodiscard]] auto IsSceneNameAvailable(const Campaign& campaign,
                                        std::string_view name,
                                        std::size_t ignore_index = kNoScene)
    -> bool;

// Переименовывает сцену; отказ при чужом имени или неверном индексе.
[[nodiscard]] auto RenameScene(Campaign& campaign, std::size_t index,
                               std::string name) -> bool;

// Разбор «СТАРОЕ НОВОЕ» для /scene rename: имя сцены может содержать
// пробелы, поэтому берётся самая длинная подходящая сцена.
struct SceneRenameRequest {
  std::size_t index = 0;
  std::string new_name;
};

[[nodiscard]] auto ParseSceneRename(const Campaign& campaign,
                                    std::string_view args)
    -> std::optional<SceneRenameRequest>;

}  // namespace dnd
