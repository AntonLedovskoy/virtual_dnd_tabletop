#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "core/bestiary.h"
#include "core/campaign.h"
#include "core/encounter.h"
#include "core/log_line.h"
#include "core/token.h"

struct SDL_Renderer;

namespace dnd {

class TextRenderer;

// Сколько строк лога помещается в нижнюю панель.
inline constexpr int kLogVisibleLines = 7;

// Размер области рисования в координатах интерфейса: при включённом
// логическом масштабе это его логические размеры, иначе — размер вывода.
auto RenderSize(SDL_Renderer* renderer, int& width, int& height) -> bool;

// Нижняя панель с общим логом стола и строкой ввода чата. scroll — на
// сколько строк прокручено вверх от самых свежих. hint — готовая контекстная
// подсказка под строкой ввода (какие клавиши показать, решает App).
// Возвращает верхнюю границу панели или 0, если она не нарисована.
auto DrawLogPanel(SDL_Renderer* renderer, TextRenderer* text,
                  const std::vector<LogLine>& lines, bool typing,
                  const std::string& input, std::string_view hint, int scroll)
    -> float;

// Порядок инициативы справа сверху для видимых клиенту токенов.
auto DrawInitiativePanel(SDL_Renderer* renderer, TextRenderer* text,
                         const std::vector<Token>& tokens,
                         const Encounter& encounter) -> void;

// Токен из панели инициативы под точкой; 0 — мимо панели.
auto InitiativeTokenAt(SDL_Renderer* renderer, TextRenderer* text,
                       const std::vector<Token>& tokens, float px, float py)
    -> TokenId;

// Что писать о запасе движения: игрок вне боя не ограничен, DM — тоже.
enum class MovementInfo : std::uint8_t {
  kHidden = 0,
  kFree = 1,
  kBudget = 2,
};

// Сведения о выбранном токене слева сверху, включая подсказки о владельце.
// Возвращает нижнюю границу панели или 0, если ничего не нарисовано.
auto DrawTokenPanel(SDL_Renderer* renderer, TextRenderer* text,
                    const Token* token, const std::string& player_name,
                    bool is_client, bool can_claim, bool can_release,
                    MovementInfo movement) -> float;

// Список игроков слева из ростера хоста, размещается на заданной координате y.
auto DrawPlayersPanel(SDL_Renderer* renderer, TextRenderer* text,
                      const std::vector<std::string>& players, float panel_y)
    -> void;

// Номер строки игрока под точкой в панели, нарисованной на panel_y; -1 —
// мимо. Раскладка та же, что у DrawPlayersPanel.
auto PlayerRowAt(SDL_Renderer* renderer, TextRenderer* text,
                 std::size_t players, float panel_y, float px, float py) -> int;

enum class SheetField : std::uint8_t {
  kAc = 0,
  kSpeed = 1,
  kInitiative = 2,
  kAttack = 3,
  kSave = 4,
  kTempHp = 5,
  kHitDie = 6,
  kCon = 7,
  kDamage = 8,
  kNotes = 9,
};
inline constexpr int kSheetFieldCount = 10;

// Модальный лист персонажа или монстра. read_only — игрок только смотрит,
// поля не подсвечиваются и не редактируются.
auto DrawSheetPanel(SDL_Renderer* renderer, TextRenderer* text,
                    const Token& token, SheetField active,
                    const std::string& ac, const std::string& speed,
                    const std::string& attack, const std::string& save,
                    const std::string& initiative, const std::string& temp_hp,
                    const std::string& hit_die, const std::string& con,
                    const std::string& damage, const std::string& notes,
                    bool read_only) -> void;

// Модальный список встроенного бестиария.
auto DrawBestiaryPanel(SDL_Renderer* renderer, TextRenderer* text,
                       const std::vector<MonsterTemplate>& bestiary) -> void;

// Модальный список состояний выбранного токена. read_only — игрок только
// смотрит, состояния и КС задаёт DM.
auto DrawConditionsPanel(SDL_Renderer* renderer, TextRenderer* text,
                         const Token& token,
                         const std::vector<Condition>& conditions,
                         bool read_only, std::size_t selected, bool edit_dc,
                         const std::string& dc_buffer) -> void;

// Модальный список сцен кампании для DM: имя, заметка, активная и выбранная.
// edit_label непустой, когда DM правит имя или заметку; status — сообщение
// об ошибке вместо подсказки.
auto DrawScenesPanel(SDL_Renderer* renderer, TextRenderer* text,
                     const std::vector<Scene>& scenes, std::size_t active,
                     std::size_t selected, const std::string& edit_label,
                     const std::string& edit_text, const std::string& status,
                     std::string_view hint) -> void;

// Сцена под точкой в модальной панели сцен; kNoScene — мимо списка.
auto SceneRowAt(SDL_Renderer* renderer, TextRenderer* text,
                const std::vector<Scene>& scenes, std::size_t selected,
                float px, float py) -> std::size_t;

// Баннер поверх стола: связь с хостом потеряна, идёт переподключение.
auto DrawConnectionBanner(SDL_Renderer* renderer, TextRenderer* text,
                          std::string_view message) -> void;

// Модальная справка по клавишам: общая часть и набор роли.
auto DrawHelpPanel(SDL_Renderer* renderer, TextRenderer* text, bool is_dm)
    -> void;

}  // namespace dnd
