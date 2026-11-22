#pragma once

#include <string>

#include "core/image_name.h"

namespace dnd {

inline constexpr int kMinBackgroundOpacity = 0;
inline constexpr int kMaxBackgroundOpacity = 100;
inline constexpr int kMinBackgroundScale = 10;
inline constexpr int kMaxBackgroundScale = 400;

// Подложка карты: картинка лежит рядом с .exe, сцена хранит только её имя.
// Клиент ищет файл у себя и без картинки рисует одну сетку.
struct MapBackground {
  std::string image;
  // Непрозрачность в процентах: 0 — картинки не видно, 100 — полностью.
  int opacity = 100;
  // Масштаб в процентах от вписанной в карту картинки.
  int scale = 100;
};

// Пустое имя означает «подложки нет» и тоже допустимо.
[[nodiscard]] auto IsValidMapBackground(const MapBackground& background)
    -> bool;

struct ChatCommand;

// Применяет разобранную директиву /map к подложке: off, opacity N, scale N
// или имя файла. false — команда не про подложку или разбор не удался.
// Общая логика игры (до запрета) и редактора карт.
auto ApplyMapCommand(MapBackground& background, const ChatCommand& command)
    -> bool;

}  // namespace dnd
