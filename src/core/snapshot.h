#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/aoe.h"
#include "core/encounter.h"
#include "core/grid_map.h"
#include "core/map_background.h"
#include "core/token.h"

namespace dnd {

// Память тумана одной стороны: что она уже исследовала на этой карте.
// Видимое множество не хранится — оно пересчитывается при загрузке сцены,
// поэтому в файле и в сети достаточно исследованных клеток.
struct FogRecord {
  // Имя игрока; пустое — общий обзор партии до выбора персонажа.
  std::string owner;
  // По клеткам карты в порядке строк: 1 — клетка разведана.
  std::vector<std::uint8_t> explored;
};

// Полное авторитетное состояние мира: достаточно, чтобы собрать GameState
// заново — и для подключающихся клиентов, и для файлов сохранения.
struct GameSnapshot {
  GridMap map{1, 1};
  std::vector<Token> tokens;
  TokenId next_token_id = 1;
  Encounter encounter;
  // Шаблон области, который DM показывает столу.
  AoeTemplate aoe;
  // Картинка под сеткой: имя файла рядом с .exe, прозрачность и масштаб.
  MapBackground background;
  // Клетки, которые DM навсегда открыл столу: их видят все игроки, даже без
  // своего персонажа рядом. Хранятся в сцене, снимаются /reveal off.
  std::vector<Cell> revealed;
  // Туман игроков на этой сцене: переживает смену сцены, сохранение и отмену.
  std::vector<FogRecord> fog;
};

}  // namespace dnd
