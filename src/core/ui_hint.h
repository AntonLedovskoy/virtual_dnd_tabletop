#pragma once

#include <cstdint>
#include <vector>

namespace dnd {

// Режим стола, который перекрывает обычную подсказку: пока открыт шаблон
// области или модальная панель, внизу нужны только их клавиши.
enum class HintMode : std::uint8_t {
  kTable,
  kAoe,
  kSheet,
  kBestiary,
  kScenes,
  kConditions,
  kActions,
  kCombat,
};

// Одно действие в нижней подсказке. Текст живёт в src/app/strings.h, чтобы
// ядро не тянуло русские строки интерфейса.
enum class Hint : std::uint8_t {
  kChat,
  kHelp,
  kExitMenu,
  kLog,
  kLogNewest,
  kJournal,
  kSelect,
  kClaim,
  kDmClaim,
  kAttack,
  kSave,
  kInitiative,
  kSheet,
  kConditions,
  kActions,
  kColor,
  kMoveRange,
  kCrawl,
  kJump,
  kRuler,
  kAoe,
  kBestiary,
  kScenes,
  kDmSpawn,
  kDmAdjust,
  kDmDelete,
  kDmCombat,
  kDmComposition,
  kDmTurn,
  kDmUndo,
  kAoeKeys,
  kSheetKeys,
  kSheetViewKeys,
  kBestiaryKeys,
  kScenesKeys,
  kConditionsKeys,
  kConditionsViewKeys,
  kActionsKeys,
  kCombatKeys,
};

// Что сейчас происходит за столом. Остальное подсказка не учитывает: ей
// хватает роли, выделения и открытой панели.
struct HintContext {
  HintMode mode = HintMode::kTable;
  bool is_dm = false;
  bool has_selection = false;
  // can_view_sheet — лист можно открыть на просмотр, даже если он чужой;
  // цвет меняет только DM (is_dm).
  bool can_view_sheet = false;
  bool can_claim = false;
  // Свободный персонаж под курсором у DM: C берёт его в ведение мастера.
  bool can_dm_claim = false;
  bool in_combat = false;
  bool log_scrolled = false;
  // Выбранный токен «сбит с ног»: показываем, как ползти.
  bool prone = false;
};

// Действия нижней подсказки в порядке показа: сначала общие клавиши, затем
// клавиши выбранного токена и только потом добавки роли.
[[nodiscard]] auto BottomHints(const HintContext& context) -> std::vector<Hint>;

}  // namespace dnd
