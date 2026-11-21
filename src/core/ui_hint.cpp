#include "core/ui_hint.h"

namespace dnd {

auto BottomHints(const HintContext& context) -> std::vector<Hint> {
  switch (context.mode) {
    case HintMode::kAoe:
      return {Hint::kAoeKeys};
    case HintMode::kSheet:
      return {Hint::kSheetKeys};
    case HintMode::kBestiary:
      return {Hint::kBestiaryKeys};
    case HintMode::kScenes:
      return {Hint::kScenesKeys};
    case HintMode::kConditions:
      return {Hint::kConditionsKeys};
    case HintMode::kTable:
      break;
  }

  std::vector<Hint> hints{Hint::kChat, Hint::kHelp, Hint::kLog};
  if (context.log_scrolled) {
    hints.push_back(Hint::kLogNewest);
  }
  if (context.can_claim) {
    hints.push_back(Hint::kClaim);
  }
  if (context.can_release) {
    hints.push_back(Hint::kRelease);
  }
  // Без выделения половина клавиш только пишет «сначала выберите токен».
  if (!context.has_selection) {
    hints.push_back(Hint::kSelect);
  } else {
    hints.insert(hints.end(),
                 {Hint::kAttack, Hint::kSave, Hint::kMoveRange, Hint::kRuler});
    if (context.can_edit) {
      hints.insert(hints.end(),
                   {Hint::kSheet, Hint::kConditions, Hint::kColor});
    }
    if (context.is_dm) {
      hints.insert(hints.end(), {Hint::kInitiative, Hint::kDmAdjust,
                                 Hint::kDmDelete, Hint::kDmCombat});
      if (context.in_combat) {
        hints.push_back(Hint::kDmTurn);
      }
    }
  }
  if (context.is_dm) {
    hints.insert(hints.end(), {Hint::kDmSpawn, Hint::kAoe, Hint::kBestiary,
                               Hint::kScenes, Hint::kDmUndo});
  }
  return hints;
}

}  // namespace dnd
