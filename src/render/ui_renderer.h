#pragma once

#include <string>
#include <vector>

#include "core/bestiary.h"
#include "core/encounter.h"
#include "core/log_line.h"
#include "core/token.h"

struct SDL_Renderer;

namespace dnd {

class TextRenderer;

// Нижняя панель с общим логом стола и строкой ввода чата.
auto DrawLogPanel(SDL_Renderer* renderer, TextRenderer* text,
                  const std::vector<LogLine>& lines, bool typing,
                  const std::string& input, bool is_dm) -> void;

// Порядок инициативы справа сверху для видимых клиенту токенов.
auto DrawInitiativePanel(SDL_Renderer* renderer, TextRenderer* text,
                         const std::vector<Token>& tokens,
                         const Encounter& encounter) -> void;

// Сведения о выбранном токене слева сверху, включая подсказки о владельце.
// Возвращает нижнюю границу панели или 0, если ничего не нарисовано.
auto DrawTokenPanel(SDL_Renderer* renderer, TextRenderer* text,
                    const Token* token, const std::string& player_name,
                    bool is_client, bool can_claim, bool can_release) -> float;

// Список игроков слева из ростера хоста, размещается на заданной координате y.
auto DrawPlayersPanel(SDL_Renderer* renderer, TextRenderer* text,
                      const std::vector<std::string>& players, float panel_y)
    -> void;

enum class SheetField : std::uint8_t {
  kAc = 0,
  kSpeed = 1,
  kAttack = 2,
  kSave = 3,
  kDamage = 4,
  kNotes = 5,
};

// Модальный лист персонажа или монстра.
auto DrawSheetPanel(SDL_Renderer* renderer, TextRenderer* text,
                    const Token& token, SheetField active,
                    const std::string& ac, const std::string& speed,
                    const std::string& attack, const std::string& save,
                    const std::string& damage, const std::string& notes)
    -> void;

// Модальный список встроенного бестиария.
auto DrawBestiaryPanel(SDL_Renderer* renderer, TextRenderer* text,
                       const std::vector<MonsterTemplate>& bestiary) -> void;

}  // namespace dnd
