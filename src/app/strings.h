#pragma once

#include <array>
#include <string_view>

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
    "/portrait | /reveal | /xp | /kick";
inline constexpr std::string_view kColorHint = "V — цвет";
inline constexpr std::string_view kChatHintDm =
    "Enter — чат | PgUp/PgDn — лог | /roll d20+5 | A атака | S спасбросок | "
    "B бестиарий | F шаблон | K состояния | L лист | M дальность | "
    "Alt+ЛКМ линейка | Tab сцены | P персонаж, N монстр, Del дважды удалить, "
    "+/- HP, [ ] иниц., T бой, G ход, Ctrl+Z отмена";
inline constexpr std::string_view kChatHintPlayer =
    "Enter — чат | PgUp/PgDn — лог | /roll d20+5 | A атака | S спасбросок | "
    "C забрать | R освободить | K состояния | L лист | M дальность | "
    "Alt+ЛКМ линейка | V цвет";
inline constexpr std::string_view kAoeHint =
    "F — выйти | ЛКМ/курсор — место | ←/→ поворот | ↑/↓ размер | "
    "1–3 вид | , . ширина | Esc убрать";
inline constexpr std::string_view kAoeDmOnly =
    "Шаблон области ставит только DM";
inline constexpr std::string_view kRejected = "Отклонено: ";
inline constexpr std::string_view kCellNotExplored = "Клетка не разведана";
inline constexpr std::string_view kSelectToken = "Сначала выберите токен";
inline constexpr std::string_view kSelectTokenForCommand =
    "Сначала выберите токен для команды";
inline constexpr std::string_view kConnectionLost =
    "Связь потеряна, переподключение...";
inline constexpr std::string_view kConnectionRestored =
    "Связь с хостом восстановлена";
inline constexpr std::string_view kLogScrolledPrefix = "Прокручено вверх: ";
inline constexpr std::string_view kLogScrolledSuffix = " стр. | End — к новым";

// Справка F1: общая часть и набор, зависящий от роли.
inline constexpr std::string_view kHelpTitle = "Справка по клавишам";
inline constexpr std::string_view kHelpCommonTitle = "Для всех";
inline constexpr std::string_view kHelpDmTitle = "Мастер (DM)";
inline constexpr std::string_view kHelpPlayerTitle = "Игрок";
inline constexpr std::string_view kHelpClose = "F1 или Esc — закрыть";
inline constexpr std::array<std::string_view, 15> kHelpCommon = {
    "Enter — чат и команды",
    "F1 — справка, Esc — закрыть",
    "ЛКМ — выбрать, перетащить; в списках — клик",
    "СКМ — панорама, колесо — зум",
    "Ctrl+= / Ctrl+- — масштаб интерфейса",
    "Home — центрировать карту",
    "Alt+ЛКМ — линейка расстояний",
    "PgUp / PgDn — лог, End — к новым",
    "Колесо над логом — прокрутка лога",
    "A / Shift+A — атака / с преимуществом",
    "S — спасбросок, I — инициатива",
    "K — состояния, L — лист",
    "M — дальность хода (5–10–5), V — цвет",
    "Команды: /damage N, /heal N",
    "/deathsave — при 0 HP",
};
inline constexpr std::array<std::string_view, 3> kHelpPlayer = {
    "C — забрать персонажа под курсором",
    "R — освободить своего персонажа",
    "/portrait файл.png — портрет своего персонажа",
};
inline constexpr std::array<std::string_view, 16> kHelpDm = {
    "P / N — персонаж / монстр под курсором",
    "Del дважды — удалить выбранный токен",
    "+ / - — HP, [ / ] — инициатива",
    "B — бестиарий (Shift+цифра — группа)",
    "Tab — сцены (R имя, E заметка)",
    "T — бой, G — следующий ход",
    "F — шаблон области, Shift+F — убрать",
    "F5 — сохранить, F9 — загрузить",
    "Команды: /rename, /owner, /maxhp, /kick",
    "/rest — короткий отдых, /longrest — длинный",
    "Enter в режиме F и /aoe 2d6 15 — урон по шаблону",
    "Ctrl+Z — отменить последнее действие DM",
    "/map карта.png — подложка, /map off — убрать",
    "/portrait файл.png — портрет выбранного токена",
    "/reveal — открыть область под курсором, /reveal off — скрыть",
    "/xp 300 — опыт выбранному, /xp 300 all — всей партии",
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
    "1–8 — переключить (10 ходов) | 0 — очистить | Esc — закрыть";
inline constexpr std::string_view kMovementLeft = "Осталось хода";
inline constexpr std::string_view kMovementOf = "из";
inline constexpr std::string_view kMovementFree =
    "Вне боя запас движения не тратится";
inline constexpr std::string_view kSheetDamage = "Урон";
inline constexpr std::string_view kSheetNotes = "Заметки";
inline constexpr std::string_view kSheetHint =
    "Tab — поле | Enter — сохранить | Esc — закрыть";
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
    "Использование: /reveal (область под курсором) или /reveal off";
inline constexpr std::string_view kRevealDmOnly =
    "Области столу открывает только DM";
inline constexpr std::string_view kXpUsage =
    "Использование: /xp 300 или /xp 300 all";
inline constexpr std::string_view kXpDmOnly = "Опыт начисляет только DM";

}  // namespace dnd::strings
