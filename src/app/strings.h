#pragma once

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
inline constexpr std::string_view kInitiativeShort = "Иниц.";
inline constexpr std::string_view kKind = "Тип";
inline constexpr std::string_view kPlayer = "персонаж";
inline constexpr std::string_view kNpc = "NPC";
inline constexpr std::string_view kMonster = "монстр";
inline constexpr std::string_view kClaimHint = "C — забрать персонажа";
inline constexpr std::string_view kReleaseHint = "R — освободить персонажа";
inline constexpr std::string_view kYourToken = "это ваш персонаж";
inline constexpr std::string_view kDmSpawnHint =
    "P — персонаж, N — монстр, B — бестиарий, Del — удалить";
inline constexpr std::string_view kDmCommandHint =
    "/rename имя | /owner имя | /maxhp N | /kick имя";
inline constexpr std::string_view kColorHint = "V — цвет";
inline constexpr std::string_view kChatHintDm =
    "Enter — чат | /roll d20+5 | A атака | S спасбросок | B бестиарий | "
    "F шаблон | K состояния | L лист | M дальность | Alt+ЛКМ линейка | "
    "Tab сцены | P персонаж, N монстр, Del удалить, +/- HP, [ ] иниц., "
    "T бой, G ход";
inline constexpr std::string_view kChatHintPlayer =
    "Enter — чат | /roll d20+5 | A атака | S спасбросок | C забрать | "
    "R освободить | K состояния | L лист | M дальность | Alt+ЛКМ линейка | "
    "V цвет";
inline constexpr std::string_view kAoeHint =
    "F — выйти | ЛКМ/курсор — место | ←/→ поворот | ↑/↓ размер | "
    "1–3 вид | , . ширина | Esc убрать";
inline constexpr std::string_view kAoeDmOnly =
    "Шаблон области ставит только DM";
inline constexpr std::string_view kRejected = "Отклонено: ";
inline constexpr std::string_view kSelectToken = "Сначала выберите токен";
inline constexpr std::string_view kSelectTokenForCommand =
    "Сначала выберите токен для команды";
inline constexpr std::string_view kConnectionLost =
    "Связь потеряна, переподключение...";
inline constexpr std::string_view kConnectionRestored =
    "Связь с хостом восстановлена";

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
    "1–5 — кисть | Tab — скрыть панель | F5 — сохранить | F9 — загрузить | "
    "Enter — играть | Esc — меню";
inline constexpr std::string_view kEditorSceneSaved = "Сцена сохранена";
inline constexpr std::string_view kEditorSceneLoaded = "Сцена загружена";
inline constexpr std::string_view kEditorSceneMissing =
    "Файл сцены не найден или повреждён";

inline constexpr std::string_view kSheetTitle = "Лист";
inline constexpr std::string_view kSheetAc = "КД";
inline constexpr std::string_view kSheetSpeed = "Скорость, м";
inline constexpr std::string_view kSheetAttack = "Бонус атаки";
inline constexpr std::string_view kSheetSave = "Бонус спасброска";
inline constexpr std::string_view kSheetTempHp = "Временные HP";
inline constexpr std::string_view kConditions = "Состояния";
inline constexpr std::string_view kMovementLeft = "Осталось хода";
inline constexpr std::string_view kMovementOf = "из";
inline constexpr std::string_view kMovementFree = "Вне боя движение свободное";
inline constexpr std::string_view kSheetDamage = "Урон";
inline constexpr std::string_view kSheetNotes = "Заметки";
inline constexpr std::string_view kSheetHint =
    "Tab — поле | Enter — сохранить | Esc — закрыть";
inline constexpr std::string_view kSheetNoAccess =
    "Нельзя редактировать чужого персонажа";
inline constexpr std::string_view kBestiaryTitle = "Бестиарий";
inline constexpr std::string_view kBestiaryHint =
    "1–5 — поставить под курсор | Esc — закрыть";
inline constexpr std::string_view kBestiaryDmOnly =
    "Бестиарий доступен только DM";

inline constexpr std::string_view kScenesTitle = "Сцены";
inline constexpr std::string_view kScenesHint =
    "1–9 — переключить | ↑ ↓ и Enter — выбрать | N — новая копия | "
    "Del — удалить | Esc — закрыть";
inline constexpr std::string_view kScenesDmOnly = "Сцены переключает только DM";
inline constexpr std::string_view kScenesEmpty = "Сцена не найдена";

}  // namespace dnd::strings
