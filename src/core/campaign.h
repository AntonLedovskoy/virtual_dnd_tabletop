#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/snapshot.h"

namespace dnd {

// Стабильный идентификатор сцены: имя и индекс меняются, id — нет, поэтому
// оверлей партии ссылается на сцены только по нему.
using SceneId = std::uint64_t;

inline constexpr std::size_t kMaxSceneNameLength = 64;
inline constexpr std::size_t kMaxSceneNoteLength = 512;
// Значение «нет сцены»: ignore_index при проверке имени и результат поиска
// сцены по id.
inline constexpr std::size_t kNoScene = static_cast<std::size_t>(-1);
// Верхняя половина диапазона — id сцен, созданных в партии. Правка кампании
// в редакторе туда не попадает, поэтому номера не сталкиваются.
inline constexpr SceneId kOverlaySceneIdBase = SceneId{1} << 63;

// Одна сцена кампании: имя, заметка DM и полное авторитетное состояние мира.
struct Scene {
  SceneId id = 0;
  std::string name;
  std::string note;
  GameSnapshot snapshot;
  // Сцена есть только в оверлее партии: в файле кампании её нет. Поле
  // заполняет ApplyOverlay, в файл оно не пишется.
  bool overlay_only = false;
};

// Кампания — набор сцен, между которыми DM переключает стол. Активная сцена
// существует всегда: scenes не пуст, а active указывает внутрь него.
struct Campaign {
  std::vector<Scene> scenes;
  std::size_t active = 0;
  // Следующий свободный id кампании: номера не переиспользуются после
  // удаления сцены, иначе оверлей старой партии попадёт в чужую сцену.
  SceneId next_scene_id = 1;
};

// Кампания из одной сцены; имя пустое заменяется на «Сцена 1».
auto MakeCampaign(GameSnapshot snapshot, std::string name) -> Campaign;

// Выдаёт следующий id кампании и сдвигает счётчик.
auto NextSceneId(Campaign& campaign) -> SceneId;

// Индекс сцены по id; kNoScene, если такой сцены нет.
[[nodiscard]] auto FindSceneIndex(const Campaign& campaign, SceneId id)
    -> std::size_t;

// Копия сцены с новым id: имя и заметка остаются, снимок копируется.
auto CopyScene(Campaign& campaign, std::size_t index) -> std::optional<Scene>;

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

// Кампания не может остаться без сцен: последнюю карту не удаляют ни панель
// DM, ни редактор.
[[nodiscard]] auto CanDeleteScene(const Campaign& campaign) -> bool;

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
