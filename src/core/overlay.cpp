#include "core/overlay.h"

#include <algorithm>
#include <cstddef>
#include <string>
#include <utility>

#include "core/navigation.h"
#include "core/utf8.h"

namespace dnd {
namespace {

auto IsPartySceneId(SceneId id) -> bool { return id >= kOverlaySceneIdBase; }

// Запись для сцены кампании создаётся при первом изменении.
auto EnsureRecord(Overlay& overlay, SceneId id) -> OverlayScene* {
  if (id == 0) {
    return nullptr;
  }
  if (OverlayScene* existing = FindOverlayScene(overlay, id)) {
    return existing;
  }
  if (overlay.scenes.size() >= kMaxOverlayScenes) {
    return nullptr;
  }
  overlay.scenes.push_back(OverlayScene{.id = id});
  return &overlay.scenes.back();
}

auto MakeUniqueName(const std::vector<std::string>& used,
                    const std::string& name) -> std::string {
  for (std::size_t attempt = 2; attempt < 10000; ++attempt) {
    const std::string suffix = " (" + std::to_string(attempt) + ")";
    std::string candidate = name;
    if (candidate.size() + suffix.size() > kMaxSceneNameLength) {
      TruncateUtf8(candidate, kMaxSceneNameLength - suffix.size());
    }
    candidate += suffix;
    if (std::ranges::find(used, candidate) == used.end()) {
      return candidate;
    }
  }
  return "Сцена";
}

}  // namespace

auto FindOverlayScene(Overlay& overlay, SceneId id) -> OverlayScene* {
  for (OverlayScene& scene : overlay.scenes) {
    if (scene.id == id) {
      return &scene;
    }
  }
  return nullptr;
}

auto FindOverlayScene(const Overlay& overlay, SceneId id)
    -> const OverlayScene* {
  for (const OverlayScene& scene : overlay.scenes) {
    if (scene.id == id) {
      return &scene;
    }
  }
  return nullptr;
}

auto NextOverlaySceneId(Overlay& overlay) -> SceneId {
  overlay.next_scene_id = std::max(overlay.next_scene_id, kOverlaySceneIdBase);
  for (const OverlayScene& scene : overlay.scenes) {
    overlay.next_scene_id = std::max(overlay.next_scene_id, scene.id + 1);
  }
  return overlay.next_scene_id++;
}

auto AddScene(Overlay& overlay, SceneId id, std::string name,
              GameSnapshot snapshot) -> bool {
  if (!IsPartySceneId(id) || name.empty() ||
      name.size() > kMaxSceneNameLength ||
      FindOverlayScene(overlay, id) != nullptr ||
      overlay.scenes.size() >= kMaxOverlayScenes) {
    return false;
  }
  overlay.scenes.push_back(OverlayScene{
      .id = id, .name = std::move(name), .snapshot = std::move(snapshot)});
  if (id >= overlay.next_scene_id) {
    overlay.next_scene_id = id + 1;
  }
  return true;
}

auto RemoveScene(Overlay& overlay, SceneId id) -> bool {
  if (id == 0) {
    return false;
  }
  OverlayScene* record = FindOverlayScene(overlay, id);
  if (IsPartySceneId(id)) {
    // Сцену партии прятать не от чего: записи просто больше нет.
    if (record == nullptr) {
      return false;
    }
    overlay.scenes.erase(
        overlay.scenes.begin() +
        static_cast<std::ptrdiff_t>(record - overlay.scenes.data()));
    return true;
  }
  if (record != nullptr) {
    *record = OverlayScene{.id = id, .deleted = true};
    return true;
  }
  if (overlay.scenes.size() >= kMaxOverlayScenes) {
    return false;
  }
  overlay.scenes.push_back(OverlayScene{.id = id, .deleted = true});
  return true;
}

auto DeletePartyScene(Campaign& merged, Overlay& overlay, std::size_t index)
    -> bool {
  if (index >= merged.scenes.size() || !CanDeleteScene(merged)) {
    return false;
  }
  const SceneId id = merged.scenes[index].id;
  if (!RemoveScene(overlay, id)) {
    return false;
  }
  merged.scenes.erase(merged.scenes.begin() +
                      static_cast<std::ptrdiff_t>(index));
  const std::size_t next = IndexAfterDelete(index, merged.scenes.size());
  merged.active = next;
  SetActive(overlay, merged.scenes[next].id);
  return true;
}

auto ClearScene(Overlay& overlay, SceneId id) -> bool {
  const OverlayScene* record = FindOverlayScene(overlay, id);
  if (record == nullptr) {
    return false;
  }
  overlay.scenes.erase(
      overlay.scenes.begin() +
      static_cast<std::ptrdiff_t>(record - overlay.scenes.data()));
  return true;
}

auto HasOverlayChanges(const Overlay& overlay) -> bool {
  return !overlay.scenes.empty();
}

auto SetSceneSnapshot(Overlay& overlay, SceneId id, GameSnapshot snapshot)
    -> bool {
  OverlayScene* record = EnsureRecord(overlay, id);
  if (record == nullptr || record->deleted) {
    return false;
  }
  record->snapshot = std::move(snapshot);
  return true;
}

auto SetSceneName(Overlay& overlay, SceneId id, std::string name) -> bool {
  if (name.empty() || name.size() > kMaxSceneNameLength) {
    return false;
  }
  OverlayScene* record = EnsureRecord(overlay, id);
  if (record == nullptr || record->deleted) {
    return false;
  }
  record->name = std::move(name);
  return true;
}

auto SetSceneNote(Overlay& overlay, SceneId id, std::string note) -> bool {
  if (note.size() > kMaxSceneNoteLength) {
    return false;
  }
  OverlayScene* record = EnsureRecord(overlay, id);
  if (record == nullptr || record->deleted) {
    return false;
  }
  record->note = std::move(note);
  return true;
}

auto SetActive(Overlay& overlay, SceneId id) -> void { overlay.active = id; }

auto ApplyOverlay(const Campaign& base, const Overlay& overlay)
    -> OverlayMerge {
  OverlayMerge result;
  result.campaign.next_scene_id = base.next_scene_id;
  // Откуда пришла каждая сцена итога: индекс в базе или kNoScene.
  std::vector<std::size_t> base_index;

  for (std::size_t index = 0; index < base.scenes.size(); ++index) {
    const Scene& scene = base.scenes[index];
    const OverlayScene* record = FindOverlayScene(overlay, scene.id);
    if (record != nullptr && record->deleted) {
      continue;
    }
    Scene merged = scene;
    if (record != nullptr) {
      if (record->name.has_value()) {
        merged.name = *record->name;
      }
      if (record->note.has_value()) {
        merged.note = *record->note;
      }
      if (record->snapshot.has_value()) {
        merged.snapshot = *record->snapshot;
      }
    }
    merged.overlay_only = false;
    result.campaign.scenes.push_back(std::move(merged));
    base_index.push_back(index);
  }

  // Сцена, которой нет в кампании (удалена в редакторе или создана в игре),
  // но в которой играли: прогресс важнее шаблона.
  for (const OverlayScene& record : overlay.scenes) {
    if (record.deleted || !record.snapshot.has_value() ||
        FindSceneIndex(base, record.id) != kNoScene) {
      continue;
    }
    Scene scene;
    scene.id = record.id;
    scene.name = record.name.value_or("Сцена");
    scene.note = record.note.value_or("");
    scene.snapshot = *record.snapshot;
    scene.overlay_only = true;
    result.campaign.scenes.push_back(std::move(scene));
    base_index.push_back(kNoScene);
  }

  if (result.campaign.scenes.empty()) {
    result.campaign = base;
    result.rolled_back = true;
    return result;
  }

  std::size_t active = 0;
  bool chosen = false;
  if (overlay.active != 0) {
    const std::size_t index = FindSceneIndex(result.campaign, overlay.active);
    if (index != kNoScene) {
      active = index;
      chosen = true;
    }
  }
  if (!chosen && base.active < base.scenes.size()) {
    const std::size_t index =
        FindSceneIndex(result.campaign, base.scenes[base.active].id);
    if (index != kNoScene) {
      active = index;
      chosen = true;
    }
  }
  if (!chosen) {
    // Активную сцену удалили: берём следующую базовую, иначе последнюю.
    for (std::size_t index = 0; index < base_index.size(); ++index) {
      if (base_index[index] != kNoScene && base_index[index] > base.active) {
        active = index;
        chosen = true;
        break;
      }
    }
  }
  if (!chosen) {
    active = result.campaign.scenes.size() - 1;
  }
  result.campaign.active = active;

  std::vector<std::string> used;
  for (Scene& scene : result.campaign.scenes) {
    if (std::ranges::find(used, scene.name) == used.end()) {
      used.push_back(scene.name);
      continue;
    }
    scene.name = MakeUniqueName(used, scene.name);
    used.push_back(scene.name);
  }
  return result;
}

}  // namespace dnd
