#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "core/bestiary.h"
#include "core/campaign.h"
#include "core/encounter.h"
#include "core/log_history.h"
#include "core/log_line.h"
#include "core/token.h"

struct SDL_Renderer;

namespace dnd {

class TextRenderer;

// Сколько строк лога помещается в нижнюю панель.
inline constexpr int kLogVisibleLines = 7;
// Сколько строк прошлых сессий видно в модальном журнале.
inline constexpr int kLogHistoryVisibleRows = 16;

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

// Модальный журнал стола: строки session.log всех сессий, найденные по
// запросу. matches — индексы в lines, scroll — на сколько строк прокручено
// вверх от самых свежих.
auto DrawLogHistoryPanel(SDL_Renderer* renderer, TextRenderer* text,
                         const std::vector<HistoryLine>& lines,
                         const std::vector<std::size_t>& matches,
                         std::string_view query, int scroll) -> void;

// Сколько строк журнала реально помещается при текущем размере окна и
// масштабе: панель ужимается, а не вылезает за экран. Нужно App, чтобы
// прокрутка считала те же строки, что и отрисовка.
auto LogHistoryVisibleRows(SDL_Renderer* renderer, TextRenderer* text) -> int;

// Порядок инициативы справа сверху для видимых клиенту участников боя.
auto DrawInitiativePanel(SDL_Renderer* renderer, TextRenderer* text,
                         const std::vector<Token>& tokens,
                         const Encounter& encounter) -> void;

// Модальный состав боя: все токены стола, `[+]` — участник. selected —
// строка списка; окно прокручивается вокруг неё.
auto DrawCombatPanel(SDL_Renderer* renderer, TextRenderer* text,
                     const std::vector<Token>& tokens,
                     const Encounter& encounter, std::size_t selected) -> void;

// Токен из панели инициативы под точкой; 0 — мимо панели.
auto InitiativeTokenAt(SDL_Renderer* renderer, TextRenderer* text,
                       const std::vector<Token>& tokens,
                       const Encounter& encounter, float px, float py)
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
                    bool is_client, bool can_claim, MovementInfo movement,
                    bool in_combat) -> float;

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
  kSwim = 2,
  kClimb = 3,
  kInitiative = 4,
  kAttack = 5,
  kSave = 6,
  kTempHp = 7,
  kHitDie = 8,
  kCon = 9,
  kRange = 10,
  kDarkvision = 11,
  kMaxHp = 12,
  kAutoHp = 13,
  kHitDiceCount = 14,
  kDamage = 15,
  kResist = 16,
  kVulnerable = 17,
  kImmune = 18,
  kNotes = 19,
  kSkill = 20,
  kProficiencies = 21,
};
inline constexpr int kSheetFieldCount = 22;

// Модальный лист персонажа или монстра. read_only — игрок только смотрит,
// поля не подсвечиваются и не редактируются.
auto DrawSheetPanel(SDL_Renderer* renderer, TextRenderer* text,
                    const Token& token, SheetField active,
                    const std::string& ac, const std::string& speed,
                    const std::string& swim, const std::string& climb,
                    const std::string& attack, const std::string& save,
                    const std::string& initiative, const std::string& temp_hp,
                    const std::string& hit_die, const std::string& con,
                    const std::string& range, const std::string& damage,
                    const std::string& resist, const std::string& vulnerable,
                    const std::string& immune, const std::string& notes,
                    const std::string& darkvision, const std::string& max_hp,
                    const std::string& auto_hp,
                    const std::string& hit_dice_count, const std::string& skill,
                    const std::string& proficiencies, bool read_only) -> void;

// Модальный список встроенного бестиария.
auto DrawBestiaryPanel(SDL_Renderer* renderer, TextRenderer* text,
                       const std::vector<MonsterTemplate>& bestiary) -> void;

// Модальный список состояний выбранного токена. read_only — игрок только
// смотрит, состояния и КС задаёт DM.
auto DrawConditionsPanel(SDL_Renderer* renderer, TextRenderer* text,
                         const Token& token, const std::vector<Token>& tokens,
                         const std::vector<Condition>& conditions,
                         bool read_only, std::size_t selected, bool edit_dc,
                         const std::string& dc_buffer, bool edit_custom,
                         const std::string& custom_buffer) -> void;

// Модальный список действий хода (2024): рывок, отход, уклонение и другие.
// own_turn — ход этого токена и действие ещё не потрачено.
auto DrawActionsPanel(SDL_Renderer* renderer, TextRenderer* text,
                      const Token& token, bool own_turn) -> void;

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

// Подтверждение выхода в стартовое меню. is_host меняет текст: хост
// предупреждает, что отключит игроков.
auto DrawExitToMenuPanel(SDL_Renderer* renderer, TextRenderer* text,
                         bool is_host) -> void;

}  // namespace dnd
