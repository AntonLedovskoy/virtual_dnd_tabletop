#pragma once

#include <string_view>

namespace dnd::strings {

// Player-facing UI strings. Kept in one place so a future language switch does
// not require touching the renderer and app logic.
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
inline constexpr std::string_view kYourToken = "это ваш персонаж";
inline constexpr std::string_view kDmSpawnHint =
    "P — персонаж, N — монстр, Del — удалить";
inline constexpr std::string_view kDmCommandHint =
    "/rename имя | /owner имя | /maxhp N | /kick имя";
inline constexpr std::string_view kColorHint = "V — цвет";
inline constexpr std::string_view kChatHint =
    "Enter — чат | /roll 2d6 | C — забрать | V — цвет | "
    "DM: N монстр, P персонаж, Del удалить, +/- HP, [ ] иниц., T бой, G ход";
inline constexpr std::string_view kRejected = "Отклонено: ";
inline constexpr std::string_view kSelectToken = "Сначала выберите токен";
inline constexpr std::string_view kSelectTokenForCommand =
    "Сначала выберите токен для команды";

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

}  // namespace dnd::strings
