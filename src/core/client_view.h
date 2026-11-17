#pragma once

#include <vector>

#include "core/encounter.h"
#include "core/event.h"
#include "core/grid_map.h"
#include "core/log_line.h"
#include "core/snapshot.h"
#include "core/token.h"
#include "core/visibility.h"

namespace dnd {

// Всё, что разрешено знать клиенту: собственная копия мира и срез
// видимости, полученный от сервера. Рендеринг читает только отсюда.
class ClientView {
 public:
  ClientView();

  // Полная (пере)синхронизация токенов и тумана, например при входе или
  // после загрузки сохранения. История лога сохраняется.
  auto Reset(GameSnapshot snapshot, const FogSnapshot& fog) -> void;
  auto Apply(const Event& event) -> void;
  // Локальная подсказка клиента, другим игрокам не отправляется.
  auto AddLocalLine(std::string text) -> void;

  [[nodiscard]] auto Map() const -> const GridMap& { return map_; }
  [[nodiscard]] auto Tokens() const -> const std::vector<Token>& {
    return tokens_;
  }
  [[nodiscard]] auto Fog() const -> const FogOfWar& { return fog_; }
  [[nodiscard]] auto Log() const -> const std::vector<LogLine>& { return log_; }
  [[nodiscard]] auto EncounterState() const -> const Encounter& {
    return encounter_;
  }

 private:
  static constexpr std::size_t kMaxLogLines = 200;

  auto FindToken(TokenId id) -> Token*;

  GridMap map_;
  std::vector<Token> tokens_;
  FogOfWar fog_;
  std::vector<LogLine> log_;
  Encounter encounter_;
};

}  // namespace dnd
