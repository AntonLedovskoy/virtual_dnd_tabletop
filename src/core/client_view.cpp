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
  map_ = std::move(snapshot.map);
  tokens_ = std::move(snapshot.tokens);
  fog_.Restore(fog);
}

auto ClientView::Apply(const Event& event) -> void {
  std::visit(
      [this](const auto& ev) -> void {
        using T = std::decay_t<decltype(ev)>;
        if constexpr (std::is_same_v<T, TokenMoved>) {
          if (Token* token = FindToken(ev.id)) {
            token->cell = ev.to;
          }
        } else if constexpr (std::is_same_v<T, ViewUpdated>) {
          Reset(ev.snapshot, ev.fog);
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
        } else if constexpr (std::is_same_v<T, LogLineAdded>) {
          log_.push_back(ev.line);
          if (log_.size() > kMaxLogLines) {
            log_.erase(log_.begin());
          }
        }
      },
      event);
}

auto ClientView::AddLocalLine(std::string text) -> void {
  log_.push_back(LogLine{
      .author = {},
      .text = std::move(text),
      .timestamp = CurrentTimeSeconds(),
  });
  if (log_.size() > kMaxLogLines) {
    log_.erase(log_.begin());
  }
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
