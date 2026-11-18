#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "core/snapshot.h"

namespace dnd {

// Одна сцена кампании: имя и полное авторитетное состояние мира.
struct Scene {
  std::string name;
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

}  // namespace dnd
