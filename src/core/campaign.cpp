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
  campaign.scenes.push_back(
      Scene{.name = std::move(name), .snapshot = std::move(snapshot)});
  campaign.active = 0;
  return campaign;
}

auto NextSceneName(const Campaign& campaign) -> std::string {
  for (std::size_t index = 1; index <= kMaxNameAttempts; ++index) {
    std::string candidate = "Сцена " + std::to_string(index);
    const bool taken = std::ranges::any_of(
        campaign.scenes, [&candidate](const Scene& scene) -> bool {
          return scene.name == candidate;
        });
    if (!taken) {
      return candidate;
    }
  }
  return "Новая сцена";
}

}  // namespace dnd
