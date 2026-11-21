#pragma once

#include <array>
#include <string_view>

#include "core/ui_hint.h"

namespace dnd::strings {

// Строки интерфейса для игрока. Собраны в одном месте, чтобы будущая смена
// языка не требовала правок рендерера и логики приложения.
inline constexpr std::string_view kInitiative = "Инициатива";
inline constexpr std::string_view kRound = "раунд";
inline constexpr std::string_view kToken = "Токен";
inline constexpr std::string_view kName = "Имя";
inline constexpr std::string_view kOwner = "Владелец";
inline constexpr std::string_view kFree = "свободен";
inline constexpr std::string_view kHp = "HP";
inline constexpr std::string_view kXpShort = "XP";
inline constexpr std::string_view kXpMax = "макс.";
inline constexpr std::string_view kHitDiceShort = "КХ";
inline constexpr std::string_view kInitiativeShort = "Иниц.";
inline constexpr std::string_view kLevelShort = "Ур.";
inline constexpr std::string_view kKind = "Тип";
inline constexpr std::string_view kPlayer = "персонаж";
inline constexpr std::string_view kNpc = "NPC";
inline constexpr std::string_view kMonster = "монстр";
inline constexpr std::string_view kClaimHint = "C — забрать персонажа";
inline constexpr std::string_view kReleaseHint = "R — освободить персонажа";
inline constexpr std::string_view kYourToken = "это ваш персонаж";
inline constexpr std::string_view kDmSpawnHint =
    "P — персонаж, N — монстр, B — бестиарий, Del дважды — удалить";
inline constexpr std::string_view kDmCommandHint =
    "/rename | /owner | /maxhp | /damage | /heal | /rest | /aoe | /map | "
    "/portrait | /reveal | /unreveal | /xp | /time | /kick";
inline constexpr std::string_view kColorHint = "V — цвет";
// Нижняя подсказка собирается из этих кусков по контексту (core/ui_hint):
// общие клавиши, клавиши выбранного токена и добавки роли.
inline constexpr std::string_view kHintChat = "Enter — чат (/roll d20+5)";
inline constexpr std::string_view kHintHelp = "F1 — справка";
inline constexpr std::string_view kHintLog = "PgUp/PgDn — лог";
inline constexpr std::string_view kHintLogNewest = "End — к новым";
inline constexpr std::string_view kHintJournal = "F3 — журнал";
inline constexpr std::string_view kHintSelect = "ЛКМ — выбрать токен";
inline constexpr std::string_view kHintAttack = "A — атака цели под курсором";
inline constexpr std::string_view kHintSave = "S — спасбросок";
inline constexpr std::string_view kHintInitiative = "I — бросок инициативы";
inline constexpr std::string_view kHintSheet = "L — лист";
inline constexpr std::string_view kHintConditions = "K — состояния";
inline constexpr std::string_view kHintMoveRange = "M — дальность";
inline constexpr std::string_view kHintRuler = "Alt+ЛКМ — линейка";
inline constexpr std::string_view kHintAoe = "F — шаблон области";
inline constexpr std::string_view kHintBestiary = "B — бестиарий";
inline constexpr std::string_view kHintScenes = "Tab — сцены";
inline constexpr std::string_view kHintSpawn = "P — персонаж, N — монстр";
inline constexpr std::string_view kHintAdjust = "+/- — HP, [ ] — инициатива";
inline constexpr std::string_view kHintDelete = "Del дважды — удалить";
inline constexpr std::string_view kHintCombat = "T — бой";
inline constexpr std::string_view kHintTurn = "G — следующий ход";
inline constexpr std::string_view kHintUndo = "Ctrl+Z — отмена";
inline constexpr std::string_view kAoeHint =
    "F — выйти | ЛКМ/курсор — место | ←/→ поворот | ↑/↓ размер | "
    "1–3 вид | , . ширина | Esc убрать";
inline constexpr std::string_view kAoeDmOnly =
    "Шаблон области ставит только DM";
inline constexpr std::string_view kRejected = "Отклонено: ";
inline constexpr std::string_view kCellNotExplored = "Клетка не разведана";
inline constexpr std::string_view kSelectToken = "Сначала выберите токен";
inline constexpr std::string_view kAttackNoTarget =
    "Наведите курсор на цель атаки";
inline constexpr std::string_view kAttackSelf = "Нельзя атаковать самого себя";
inline constexpr std::string_view kAttackModes =
    "Нельзя одновременно преимущество и помеху";
inline constexpr std::string_view kSelectTokenForCommand =
    "Сначала выберите токен для команды";
inline constexpr std::string_view kConnectionLost =
    "Связь потеряна, переподключение...";
inline constexpr std::string_view kConnectionRestored =
    "Связь с хостом восстановлена";
inline constexpr std::string_view kLogScrolledPrefix = "Прокручено вверх: ";
inline constexpr std::string_view kLogScrolledSuffix = " стр. | End — к новым";

// Журнал стола: прошлые и текущая сессии из session.log с поиском.
inline constexpr std::string_view kLogHistoryTitle = "Журнал стола";
inline constexpr std::string_view kLogHistorySearch = "Поиск: ";
inline constexpr std::string_view kLogHistoryEmpty =
    "В session.log пока нет строк";
inline constexpr std::string_view kLogHistoryCount = "Строк: ";
inline constexpr std::string_view kLogHistoryFound = "Найдено: ";
inline constexpr std::string_view kLogHistoryOf = " из ";
inline constexpr std::string_view kLogHistorySessionPrefix = "=== сессия ";
inline constexpr std::string_view kLogHistorySessionSuffix = " ===";
inline constexpr std::string_view kLogHistoryHint =
    "Печать — поиск | Backspace — стереть | ↑ ↓ / PgUp PgDn / колесо — "
    "прокрутка | End — к новым | Esc — закрыть";

// Справка F1: общая часть и набор, зависящий от роли.
inline constexpr std::string_view kHelpTitle = "Справка по клавишам";
inline constexpr std::string_view kHelpCommonTitle = "Для всех";
inline constexpr std::string_view kHelpDmTitle = "Мастер (DM)";
inline constexpr std::string_view kHelpPlayerTitle = "Игрок";
inline constexpr std::string_view kHelpClose = "F1 или Esc — закрыть";
inline constexpr std::array<std::string_view, 16> kHelpCommon = {
    "Enter — чат и команды",
    "F1 — справка, Esc — закрыть",
    "F3 — журнал стола с поиском и прошлыми сессиями",
    "ЛКМ — выбрать, перетащить; в списках — клик",
    "СКМ — панорама, колесо — зум",
    "Ctrl+= / Ctrl+- — масштаб интерфейса",
    "Home — центрировать карту",
    "Alt+ЛКМ — линейка расстояний",
    "PgUp / PgDn — лог, End — к новым",
    "Колесо над логом — прокрутка лога",
    "A — атака цели под курсором, Shift+A — преимущество, Ctrl+A — помеха",
    "S — спасбросок, I — инициатива",
    "K — состояния, L — лист (правит DM)",
    "M — дальность хода (5–10–5), V — цвет",
    "Команды: /damage N, /heal N",
    "/deathsave — при 0 HP",
};
inline constexpr std::array<std::string_view, 3> kHelpPlayer = {
    "C — забрать персонажа под курсором",
    "R — освободить своего персонажа",
    "/portrait файл.png — портрет своего персонажа",
};
inline constexpr std::array<std::string_view, 17> kHelpDm = {
    "P / N — персонаж / монстр под курсором",
    "Del дважды — удалить выбранный токен",
    "+ / - — HP, [ / ] — инициатива",
    "B — бестиарий (Shift+цифра — группа)",
    "Tab — сцены (R имя, E заметка)",
    "T — бой, G — следующий ход",
    "F — шаблон области, Shift+F — убрать",
    "F5 — сохранить, F9 — загрузить",
    "Команды: /rename, /owner, /maxhp, /kick",
    "/rest — короткий отдых (тратит кости хитов), /longrest — длинный",
    "Enter в режиме F и /aoe 2d6 15 — урон по шаблону",
    "Ctrl+Z — отменить последнее действие DM",
    "/map карта.png — подложка, /map off — убрать",
    "/portrait файл.png — портрет выбранного токена",
    "/reveal и /unreveal — область под курсором, /reveal off — всё",
    "/xp 300 — опыт выбранному, /xp 300 all — всей партии",
    "/time 10 — прокрутить 10 минут вне боя (s/m/h)",
};

inline constexpr std::string_view kMenuTitle = "DND";
inline constexpr std::string_view kMenuSubtitle = "Виртуальный стол";
inline constexpr std::string_view kHostTab = "Хост";
inline constexpr std::string_view kJoinTab = "Игрок";
inline constexpr std::string_view kStartHost = "Начать игру";
inline constexpr std::string_view kConnect = "Подключиться";
inline constexpr std::string_view kAddress = "Адрес хоста";
inline constexpr std::string_view kPort = "Порт";
inline constexpr std::string_view kPlayerName = "Имя";
inline constexpr std::string_view kPassword = "Пароль";
inline constexpr std::string_view kPasswordOptional = "Пароль (необязательно)";
inline constexpr std::string_view kHostAddress = "Адрес для друзей:";
inline constexpr std::string_view kCampaignScene = "Сцена: ";
inline constexpr std::string_view kCampaignSceneCount = "сцен: ";
inline constexpr std::string_view kCampaignFile = "Кампания: ";
inline constexpr std::string_view kCampaignHint =
    "↑ ↓ — выбрать кампанию | клик по строке — следующая";
inline constexpr std::string_view kNoAddress = "IP не определён";
inline constexpr std::string_view kFirewallHint =
    "Откройте TCP-порт в Windows Firewall и на роутере. "
    "Для интернета используйте публичный IP.";
inline constexpr std::string_view kTabHint =
    "← → — вкладка | Tab — следующее поле | Enter — продолжить";
inline constexpr std::string_view kConnecting = "Подключение...";
inline constexpr std::string_view kPlayers = "Игроки";

inline constexpr std::string_view kEditorTab = "Карта";
inline constexpr std::string_view kOpenEditor = "Открыть редактор";
inline constexpr std::string_view kEditorTitle = "Редактор карты";
inline constexpr std::string_view kEditorDescription =
    "Создайте карту, расставьте стены и стартовых персонажей.";
inline constexpr std::string_view kEditorSize = "Размер";
inline constexpr std::string_view kEditorBrush = "Кисть";
inline constexpr std::string_view kBrushWall = "стена";
inline constexpr std::string_view kBrushFloor = "пол";
inline constexpr std::string_view kBrushPlayer = "персонаж";
inline constexpr std::string_view kBrushMonster = "монстр";
inline constexpr std::string_view kBrushErase = "удалить";
inline constexpr std::string_view kEditorSizeHint =
    "[ / ] — ширина карты, - / = — высота";
inline constexpr std::string_view kEditorMouseHint =
    "ЛКМ — рисовать | ПКМ — удалить | СКМ — панорама | колесо — зум";
inline constexpr std::string_view kEditorKeyHint =
    "1–5 — кисть | Tab — сцены | F2 — панель | F5 — сохранить | "
    "F9 — загрузить | Enter — играть | Esc — меню";
inline constexpr std::string_view kEditorScenesHint =
    "↑ ↓ — выбрать | Enter — открыть | 1–9 — сцена | N — новая из этой "
    "карты | F5 — сохранить | Esc — закрыть";
inline constexpr std::string_view kEditorSceneOpened = "Открыта сцена: ";
inline constexpr std::string_view kEditorSceneCreated = "Создана сцена: ";
inline constexpr std::string_view kEditorSceneSaved = "Сцена сохранена";
inline constexpr std::string_view kEditorSceneLoaded = "Сцена загружена";
inline constexpr std::string_view kEditorSceneMissing =
    "Файл сцены не найден или повреждён";

inline constexpr std::string_view kSheetTitle = "Лист";
inline constexpr std::string_view kSheetAc = "КД";
inline constexpr std::string_view kSheetSpeed = "Скорость, м";
inline constexpr std::string_view kSheetAttack = "Бонус атаки";
inline constexpr std::string_view kSheetSave = "Бонус спасброска";
inline constexpr std::string_view kSheetInitiative = "Бонус инициативы";
inline constexpr std::string_view kSheetTempHp = "Временные HP";
inline constexpr std::string_view kSheetHitDie = "Кость хитов (d6–d12)";
inline constexpr std::string_view kSheetCon = "Мод. Телосложения";
inline constexpr std::string_view kSheetProficiency = "Мастерство";
inline constexpr std::string_view kSheetPerLevel = "HP за уровень";
inline constexpr std::string_view kSheetLevel = "Уровень";
inline constexpr std::string_view kSheetXp = "Опыт";
inline constexpr std::string_view kConditions = "Состояния";
inline constexpr std::string_view kConditionsHint =
    "1–8 — вкл/выкл | ↑↓ — выбор | C — КС | Esc — закрыть";
inline constexpr std::string_view kConditionsPanelHint =
    "1–8 — вкл/выкл | Shift+1–8 — до снятия | 0 — очистить";
inline constexpr std::string_view kConditionsPanelHint2 =
    "↑↓ — выбор | C — КС спасброска | Esc — закрыть";
inline constexpr std::string_view kConditionsViewHint =
    "Только просмотр, состояния меняет DM | Esc — закрыть";
inline constexpr std::string_view kConditionDcPrompt = "КС (0–30): ";
inline constexpr std::string_view kConditionDcNeedActive =
    "Сначала включите состояние";
inline constexpr std::string_view kMovementLeft = "Осталось хода";
inline constexpr std::string_view kMovementOf = "из";
inline constexpr std::string_view kMovementFree =
    "Вне боя запас движения не тратится";
inline constexpr std::string_view kSheetDamage = "Урон";
inline constexpr std::string_view kSheetNotes = "Заметки";
inline constexpr std::string_view kSheetHint =
    "Tab — поле | Enter — сохранить | Esc — закрыть";
inline constexpr std::string_view kSheetViewHint =
    "Только просмотр, лист правит DM | Esc — закрыть";
inline constexpr std::string_view kSheetBadHitDie =
    "Кость хитов: 6, 8, 10 или 12";
inline constexpr std::string_view kSheetBadCon =
    "Модификатор Телосложения: от -20 до 20";
inline constexpr std::string_view kSheetNoAccess =
    "Нельзя редактировать чужого персонажа";
inline constexpr std::string_view kBestiaryTitle = "Бестиарий";
inline constexpr std::string_view kBestiaryHint =
    "1–9 — поставить одного | Shift+1–9 — группа 3–5 | Esc — закрыть";
inline constexpr std::string_view kBestiaryDmOnly =
    "Бестиарий доступен только DM";

inline constexpr std::string_view kScenesTitle = "Сцены";
inline constexpr std::string_view kScenesHint =
    "1–9 — переключить | ↑ ↓ и Enter — выбрать | R — имя | E — заметка | "
    "N — новая копия | Del дважды — удалить | Esc — закрыть";
inline constexpr std::string_view kScenesDmOnly = "Сцены переключает только DM";
inline constexpr std::string_view kScenesEmpty = "Сцена не найдена";
inline constexpr std::string_view kSceneNameLabel = "Новое имя: ";
inline constexpr std::string_view kSceneNoteLabel = "Заметка: ";
inline constexpr std::string_view kSceneNameEmpty =
    "Имя сцены не может быть пустым";
inline constexpr std::string_view kSceneNameTaken = "Имя сцены уже занято";
inline constexpr std::string_view kSceneRenameUsage =
    "Использование: /scene rename СТАРОЕ НОВОЕ";
inline constexpr std::string_view kDeleteConfirm =
    "Нажмите Del ещё раз в течение 3 секунд";
inline constexpr std::string_view kUndoDone = "Действие отменено";
inline constexpr std::string_view kUndoEmpty = "Нечего отменять";
inline constexpr std::string_view kUndoDmOnly = "Отмена доступна только DM";

inline constexpr std::string_view kMapDmOnly =
    "Подложку карты меняет только DM";
inline constexpr std::string_view kMapUsage =
    "Использование: /map карта.png | /map off | /map opacity 60 | "
    "/map scale 150";
inline constexpr std::string_view kMapMissingImage = "Нет файла подложки: ";
inline constexpr std::string_view kPortraitUsage =
    "Использование: /portrait файл.png или /portrait off";

// Подсказки для чат-команд, которые не удалось разобрать (ChatCommandError).
inline constexpr std::string_view kRollUsage =
    "Использование: /roll d20+5, 2d6+3, adv d20+5";
inline constexpr std::string_view kKickUsage = "Использование: /kick имя";
inline constexpr std::string_view kKickDmOnly = "Игроков исключает только DM";
inline constexpr std::string_view kRenameUsage = "Использование: /rename имя";
inline constexpr std::string_view kMaxHpUsage = "Использование: /maxhp 20";
inline constexpr std::string_view kDamageUsage =
    "Использование: /damage 7 или /heal 5";
inline constexpr std::string_view kAoeUsage =
    "Использование: /aoe 2d6 15 (урон и сложность спасброска)";
inline constexpr std::string_view kRevealUsage =
    "Использование: /reveal или /unreveal (область под курсором), "
    "/reveal off — скрыть всё";
inline constexpr std::string_view kRevealDmOnly =
    "Области столу открывает только DM";
inline constexpr std::string_view kXpUsage =
    "Использование: /xp 300 или /xp 300 all";
inline constexpr std::string_view kXpDmOnly = "Опыт начисляет только DM";
inline constexpr std::string_view kTimeUsage =
    "Использование: /time 10 (минуты), /time 30s или /time 2h";
inline constexpr std::string_view kTimeDmOnly =
    "Время вне боя двигает только DM";

// Текст одного действия нижней подсказки. Какие действия показывать и в каком
// порядке, решает core/ui_hint, поэтому строки остаются здесь.
[[nodiscard]] constexpr auto HintLabel(Hint hint) -> std::string_view {
  switch (hint) {
    case Hint::kChat:
      return kHintChat;
    case Hint::kHelp:
      return kHintHelp;
    case Hint::kLog:
      return kHintLog;
    case Hint::kLogNewest:
      return kHintLogNewest;
    case Hint::kJournal:
      return kHintJournal;
    case Hint::kSelect:
      return kHintSelect;
    case Hint::kClaim:
      return kClaimHint;
    case Hint::kRelease:
      return kReleaseHint;
    case Hint::kAttack:
      return kHintAttack;
    case Hint::kSave:
      return kHintSave;
    case Hint::kInitiative:
      return kHintInitiative;
    case Hint::kSheet:
      return kHintSheet;
    case Hint::kConditions:
      return kHintConditions;
    case Hint::kColor:
      return kColorHint;
    case Hint::kMoveRange:
      return kHintMoveRange;
    case Hint::kRuler:
      return kHintRuler;
    case Hint::kAoe:
      return kHintAoe;
    case Hint::kBestiary:
      return kHintBestiary;
    case Hint::kScenes:
      return kHintScenes;
    case Hint::kDmSpawn:
      return kHintSpawn;
    case Hint::kDmAdjust:
      return kHintAdjust;
    case Hint::kDmDelete:
      return kHintDelete;
    case Hint::kDmCombat:
      return kHintCombat;
    case Hint::kDmTurn:
      return kHintTurn;
    case Hint::kDmUndo:
      return kHintUndo;
    case Hint::kAoeKeys:
      return kAoeHint;
    case Hint::kSheetKeys:
      return kSheetHint;
    case Hint::kSheetViewKeys:
      return kSheetViewHint;
    case Hint::kBestiaryKeys:
      return kBestiaryHint;
    case Hint::kScenesKeys:
      return kScenesHint;
    case Hint::kConditionsKeys:
      return kConditionsHint;
    case Hint::kConditionsViewKeys:
      return kConditionsViewHint;
  }
  return {};
}

}  // namespace dnd::strings
