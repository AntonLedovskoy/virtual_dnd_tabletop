#pragma once

#include <vector>

#include "core/light.h"
#include "core/token.h"

namespace dnd {

// Источники света сцены: каждый токен с ненулевым светом, включая монстров —
// факел освещает комнату, даже если носителя не видно. Общий список для
// GameServer (туман) и редактора (подсветка карты).
[[nodiscard]] inline auto TokenLightSources(const std::vector<Token>& tokens)
    -> std::vector<LightSource> {
  std::vector<LightSource> sources;
  for (const Token& token : tokens) {
    if (token.light_bright_cells > 0 || token.light_dim_cells > 0) {
      sources.push_back(LightSource{.cell = token.cell,
                                    .bright_cells = token.light_bright_cells,
                                    .dim_cells = token.light_dim_cells});
    }
  }
  return sources;
}

}  // namespace dnd
