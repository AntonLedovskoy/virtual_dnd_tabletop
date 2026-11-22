#include "core/campaign.h"

#include <algorithm>
#include <utility>

namespace dnd {
namespace {

constexpr std::size_t kMaxNameAttempts = 1000;

}  // namespace

auto MakeCampaign(GameSnapshot snapshot, std::string name) -> Campaign {
  if (name.empty()) {
    name = "Сцена 1";
  }
  Campaign campaign;
  campaign.scenes.push_back(Scene{.id = NextSceneId(campaign),
                                  .name = std::move(name),
                                  .snapshot = std::move(snapshot)});
  campaign.active = 0;
  return campaign;
}

auto NextSceneId(Campaign& campaign) -> SceneId {
  SceneId max_id = 0;
  for (const Scene& scene : campaign.scenes) {
    max_id = std::max(max_id, scene.id);
  }
  if (campaign.next_scene_id <= max_id ||
      campaign.next_scene_id >= kOverlaySceneIdBase) {
    campaign.next_scene_id = max_id + 1;
  }
  return campaign.next_scene_id++;
}

auto FindSceneIndex(const Campaign& campaign, SceneId id) -> std::size_t {
  if (id == 0) {
    return kNoScene;
  }
  for (std::size_t index = 0; index < campaign.scenes.size(); ++index) {
    if (campaign.scenes[index].id == id) {
      return index;
    }
  }
  return kNoScene;
}

auto CopyScene(Campaign& campaign, std::size_t index) -> std::optional<Scene> {
  if (index >= campaign.scenes.size()) {
    return std::nullopt;
  }
  Scene copy = campaign.scenes[index];
  copy.id = NextSceneId(campaign);
  copy.overlay_only = false;
  return copy;
}

auto NextSceneName(const Campaign& campaign) -> std::string {
  for (std::size_t index = 1; index <= kMaxNameAttempts; ++index) {
    std::string candidate = "Сцена " + std::to_string(index);
    if (IsSceneNameAvailable(campaign, candidate)) {
      return candidate;
    }
  }
  return "Новая сцена";
}

auto IsSceneNameAvailable(const Campaign& campaign, std::string_view name,
                          std::size_t ignore_index) -> bool {
  if (name.empty() || name.size() > kMaxSceneNameLength) {
    return false;
  }
  for (std::size_t index = 0; index < campaign.scenes.size(); ++index) {
    if (index != ignore_index && campaign.scenes[index].name == name) {
      return false;
    }
  }
  return true;
}

auto RenameScene(Campaign& campaign, std::size_t index, std::string name)
    -> bool {
  if (index >= campaign.scenes.size() ||
      !IsSceneNameAvailable(campaign, name, index)) {
    return false;
  }
  campaign.scenes[index].name = std::move(name);
  return true;
}

auto CanDeleteScene(const Campaign& campaign) -> bool {
  return campaign.scenes.size() > 1;
}

}  // namespace dnd
