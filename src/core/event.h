#pragma once

#include <variant>

#include "core/encounter.h"
#include "core/grid_map.h"
#include "core/log_line.h"
#include "core/snapshot.h"
#include "core/token.h"
#include "core/visibility.h"

namespace dnd {

// Факты, которые сервер отправляет клиенту. Только события могут менять
// отображение у клиента, поэтому локальный и сетевой пути совпадают.
struct TokenMoved {
  TokenId id = 0;
  Cell from{};
  Cell to{};
};

// Полное обновление вида игрока: туман и ровно те токены, которые игрок
// может видеть. Токены могут появиться впервые (монстр входит в свет) или
// снова исчезнуть, поэтому одного обновления тумана недостаточно.
struct ViewUpdated {
  GameSnapshot snapshot;
  FogSnapshot fog;
};

struct LogLineAdded {
  LogLine line;
};

// Инкрементальные обновления токенов. Игроки игнорируют события для
// невидимых токенов, поэтому скрытые существа не утекают.
struct TokenAdded {
  Token token;
};

struct TokenRemoved {
  TokenId id = 0;
};

struct TokenUpdated {
  Token token;
};

// Изменился боевой порядок (начало, конец, следующий ход).
struct EncounterUpdated {
  Encounter encounter;
};

using Event = std::variant<TokenMoved, ViewUpdated, LogLineAdded, TokenAdded,
                           TokenRemoved, TokenUpdated, EncounterUpdated>;

}  // namespace dnd
