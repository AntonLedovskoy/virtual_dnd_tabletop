#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "core/campaign.h"

namespace dnd {

// Сколько изменённых сцен допускает партия.
inline constexpr std::size_t kMaxOverlayScenes = 256;

// Изменение одной сцены относительно файла кампании. Запись появляется при
// первом изменении: снимок сыгранной сцены, переопределение имени или заметки,
// tombstone удалённой сцены.
struct OverlayScene {
  SceneId id = 0;
  bool deleted = false;
  std::optional<std::string> name;
  std::optional<std::string> note;
  std::optional<GameSnapshot> snapshot;
};

// Партия: отпечаток кампании, с которой она начата, изменения сцен и активная
// сцена по id. Сам файл кампании при этом не меняется.
struct Overlay {
  std::uint64_t campaign_fingerprint = 0;
  // Активная сцена по id; 0 — партия ещё не выбрала сцену.
  SceneId active = 0;
  SceneId next_scene_id = kOverlaySceneIdBase;
  std::vector<OverlayScene> scenes;
};

auto FindOverlayScene(Overlay& overlay, SceneId id) -> OverlayScene*;
auto FindOverlayScene(const Overlay& overlay, SceneId id)
    -> const OverlayScene*;

// Следующий свободный id сцены партии; номера не переиспользуются.
auto NextOverlaySceneId(Overlay& overlay) -> SceneId;

// Сцена, созданная в партии: id из диапазона оверлея, имя и снимок.
auto AddScene(Overlay& overlay, SceneId id, std::string name,
              GameSnapshot snapshot) -> bool;

// Удаление: базовая сцена получает tombstone, сцена партии исчезает.
auto RemoveScene(Overlay& overlay, SceneId id) -> bool;

// «Сбросить сцену к кампании»: запись партии стирается без tombstone, сцена
// снова берётся из файла кампании. false — записи и так не было.
auto ClearScene(Overlay& overlay, SceneId id) -> bool;

// В партии есть что продолжать: изменённая или сыгранная сцена.
[[nodiscard]] auto HasOverlayChanges(const Overlay& overlay) -> bool;

// Снимок сыгранной сцены; сцена удалена — отказ.
auto SetSceneSnapshot(Overlay& overlay, SceneId id, GameSnapshot snapshot)
    -> bool;
auto SetSceneName(Overlay& overlay, SceneId id, std::string name) -> bool;
auto SetSceneNote(Overlay& overlay, SceneId id, std::string note) -> bool;
auto SetActive(Overlay& overlay, SceneId id) -> void;

// Итог слияния кампании с партией.
struct OverlayMerge {
  Campaign campaign;
  // Оверлей удалил все сцены: вернулись к кампании и предупреждаем DM.
  bool rolled_back = false;
};

// Сцены партии поверх кампании по id. База не меняется. Tombstone скрывает
// базовую сцену; сыгранная сцена, которой нет в кампании, остаётся с пометкой
// Scene::overlay_only; дубль имени получает суффикс.
auto ApplyOverlay(const Campaign& base, const Overlay& overlay) -> OverlayMerge;

}  // namespace dnd
