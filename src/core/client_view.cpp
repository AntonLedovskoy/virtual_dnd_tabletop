#include "core/client_view.h"

#include <algorithm>
#include <type_traits>
#include <utility>

#include "core/clock.h"

namespace dnd {

ClientView::ClientView() : map_(1, 1), fog_(1, 1) {}

auto ClientView::Reset(GameSnapshot snapshot, const FogSnapshot& fog) -> void {
  encounter_ = std::move(snapshot.encounter);
  aoe_ = snapshot.aoe;
  background_ = std::move(snapshot.background);
  map_ = std::move(snapshot.map);
  tokens_ = std::move(snapshot.tokens);
  fog_.Restore(fog);
  // Мир пересобран: старые метки указывали на прежнюю карту.
  pings_.clear();
  pending_move_.reset();
}

auto ClientView::Apply(const Event& event) -> void {
  std::visit(
      [this](const auto& ev) -> void {
        using T = std::decay_t<decltype(ev)>;
        if constexpr (std::is_same_v<T, TokenMoved>) {
          if (Token* token = FindToken(ev.id)) {
            token->cell = ev.to;
          }
          // Сервер ответил на свой ход: предугадывание больше не нужно.
          // Ход на другую клетку — это ответ на более раннюю команду, и
          // прогноз последнего перетаскивания должен дождаться своего ответа.
          if (pending_move_.has_value() && pending_move_->id == ev.id &&
              pending_move_->cell == ev.to) {
            pending_move_.reset();
          }
        } else if constexpr (std::is_same_v<T, ViewUpdated>) {
          Reset(ev.snapshot, ev.fog);
        } else if constexpr (std::is_same_v<T, FogChanged>) {
          for (const FogCell& cell : ev.cells) {
            fog_.SetCell(cell.cell, cell.visible, cell.explored);
          }
        } else if constexpr (std::is_same_v<T, TokenAdded>) {
          if (FindToken(ev.token.id) == nullptr) {
            tokens_.push_back(ev.token);
          }
        } else if constexpr (std::is_same_v<T, TokenRemoved>) {
          std::erase_if(tokens_, [&ev](const Token& token) -> bool {
            return token.id == ev.id;
          });
        } else if constexpr (std::is_same_v<T, TokenUpdated>) {
          if (Token* token = FindToken(ev.token.id)) {
            *token = ev.token;
          }
        } else if constexpr (std::is_same_v<T, EncounterUpdated>) {
          encounter_ = ev.encounter;
        } else if constexpr (std::is_same_v<T, AoeUpdated>) {
          aoe_ = ev.aoe;
        } else if constexpr (std::is_same_v<T, MapBackgroundUpdated>) {
          background_ = ev.background;
        } else if constexpr (std::is_same_v<T, PingMarker>) {
          pings_.push_back(MapPing{.cell = ev.cell,
                                   .author = ev.author,
                                   .created_at = CurrentTimeSeconds()});
          if (pings_.size() > kMaxMapPings) {
            pings_.erase(pings_.begin());
          }
        } else if constexpr (std::is_same_v<T, LogLineAdded>) {
          log_.push_back(ev.line);
          ++appended_log_lines_;
          if (log_.size() > kMaxLogLines) {
            log_.erase(log_.begin());
          }
        }
      },
      event);
}

auto ClientView::ClearLog() -> void { log_.clear(); }

auto ClientView::AddLocalLine(std::string text) -> void {
  log_.push_back(LogLine{
      .author = {},
      .text = std::move(text),
      .timestamp = CurrentTimeSeconds(),
  });
  ++appended_log_lines_;
  if (log_.size() > kMaxLogLines) {
    log_.erase(log_.begin());
  }
}

auto ClientView::PredictMove(TokenId id, Cell cell, std::uint32_t now) -> void {
  pending_move_ = PendingMove{.id = id, .cell = cell, .started_at = now};
}

auto ClientView::ClearPendingMove() -> void { pending_move_.reset(); }

auto ClientView::ExpirePendingMove(std::uint32_t now) -> void {
  if (pending_move_.has_value() && now >= pending_move_->started_at &&
      now - pending_move_->started_at >= kPendingMoveSeconds) {
    pending_move_.reset();
  }
}

auto ClientView::PendingMoveCell(TokenId id) const -> std::optional<Cell> {
  if (!pending_move_.has_value() || pending_move_->id != id) {
    return std::nullopt;
  }
  return pending_move_->cell;
}

auto ClientView::PrunePings(std::uint32_t now) -> void {
  std::erase_if(pings_, [now](const MapPing& ping) -> bool {
    return now >= ping.created_at && now - ping.created_at >= kPingSeconds;
  });
}

auto ClientView::FindToken(TokenId id) -> Token* {
  for (auto& token : tokens_) {
    if (token.id == id) {
      return &token;
    }
  }
  return nullptr;
}

}  // namespace dnd
