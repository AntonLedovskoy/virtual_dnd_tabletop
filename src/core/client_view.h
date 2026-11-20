#pragma once

#include <vector>

#include "core/aoe.h"
#include "core/encounter.h"
#include "core/event.h"
#include "core/grid_map.h"
#include "core/log_line.h"
#include "core/snapshot.h"
#include "core/token.h"
#include "core/visibility.h"

namespace dnd {

// Сколько секунд на карте живёт метка «сюда» и сколько их держим разом.
inline constexpr std::uint32_t kPingSeconds = 4;
inline constexpr std::size_t kMaxMapPings = 8;

// Метка «сюда» от любого участника; поле created_at — секунды Unix.
struct MapPing {
  Cell cell{};
  std::string author;
  std::uint32_t created_at = 0;
};

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
  // Сколько строк лога пришло за всё время: по этой величине App понимает,
  // какие строки ещё не записаны в файл сессии.
  [[nodiscard]] auto AppendedLogLines() const -> std::size_t {
    return appended_log_lines_;
  }
  [[nodiscard]] auto EncounterState() const -> const Encounter& {
    return encounter_;
  }
  [[nodiscard]] auto Aoe() const -> const AoeTemplate& { return aoe_; }
  [[nodiscard]] auto Pings() const -> const std::vector<MapPing>& {
    return pings_;
  }
  // Убирает метки, которым больше kPingSeconds.
  auto PrunePings(std::uint32_t now) -> void;

 private:
  static constexpr std::size_t kMaxLogLines = 200;

  auto FindToken(TokenId id) -> Token*;

  GridMap map_;
  std::vector<Token> tokens_;
  FogOfWar fog_;
  std::vector<LogLine> log_;
  std::size_t appended_log_lines_ = 0;
  Encounter encounter_;
  AoeTemplate aoe_;
  std::vector<MapPing> pings_;
};

}  // namespace dnd
