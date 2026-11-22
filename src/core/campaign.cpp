#include "core/campaign.h"

#include <algorithm>
#include <utility>

namespace dnd {
namespace {

constexpr std::size_t kMaxNameAttempts = 1000;

auto TrimText(std::string_view text) -> std::string_view {
  while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
    text.remove_prefix(1);
  }
  while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) {
    text.remove_suffix(1);
  }
  return text;
}

}  // namespace

auto MakeCampaign(GameSnapshot snapshot, std::string name) -> Campaign {
  if (name.empty()) {
    name = "Сцена 1";
  }
  Campaign campaign;
  campaign.scenes.push_back(
      Scene{.name = std::move(name), .snapshot = std::move(snapshot)});
  campaign.active = 0;
  return campaign;
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

auto ParseSceneRename(const Campaign& campaign, std::string_view args)
    -> std::optional<SceneRenameRequest> {
  args = TrimText(args);
  std::size_t best = kNoScene;
  std::size_t best_length = 0;
  for (std::size_t index = 0; index < campaign.scenes.size(); ++index) {
    const std::string& name = campaign.scenes[index].name;
    if (name.size() > best_length && args.size() > name.size() &&
        args.starts_with(name) && args[name.size()] == ' ') {
      best = index;
      best_length = name.size();
    }
  }
  if (best == kNoScene) {
    return std::nullopt;
  }
  return SceneRenameRequest{
      .index = best,
      .new_name = std::string(TrimText(args.substr(best_length + 1))),
  };
}

}  // namespace dnd
