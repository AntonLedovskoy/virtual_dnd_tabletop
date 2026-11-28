#pragma once

#include <SDL3/SDL.h>

#include <chrono>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/autosave.h"
#include "core/camera.h"
#include "core/campaign.h"
#include "core/campaign_io.h"
#include "core/campaign_save.h"
#include "core/chat_command.h"
#include "core/chat_input.h"
#include "core/client_view.h"
#include "core/delete_confirm.h"
#include "core/editor_paint.h"
#include "core/game_server.h"
#include "core/local_transport.h"
#include "core/log_history.h"
#include "core/log_scroll.h"
#include "core/menu_model.h"
#include "core/overlay.h"
#include "core/overlay_io.h"
#include "core/overlay_save.h"
#include "core/session_log.h"
#include "core/status_message.h"
#include "net/net_client.h"
#include "net/net_server.h"
#include "render/editor_renderer.h"
#include "render/menu_renderer.h"
#include "render/ui_renderer.h"

struct TTF_Font;

namespace dnd {

class TextRenderer;
class UpdateService;
struct MenuUpdate;

enum class AppMode : std::uint8_t { kHost, kClient };

struct AppOptions {
  AppMode mode = AppMode::kHost;
  std::string connect_host = "127.0.0.1";
  std::uint16_t port = 7777;
  std::string player_name = "player";
  std::string password;
  bool auto_start = false;
  bool start_editor = false;
  // `--no-update-check`: зарезервирован под проверку обновлений (7.3), пока
  // ни на что не влияет.
  bool no_update_check = false;
};

// Штрих кисти в редакторе: зажатая ЛКМ рисует, ПКМ стирает.
enum class EditorPaintMode : std::uint8_t {
  kNone = 0,
  kBrush = 1,
  kErase = 2,
};

// Поля инспектора клетки в редакторе: Tab/↑↓ выбирают, ←→ меняют.
enum class InspectorField : std::uint8_t {
  kWater = 0,
  kCurrentDir = 1,
  kCurrentStrength = 2,
  kHazard = 3,
  kRoad = 4,
  kClimb = 5,
  kSlippery = 6,
  kCoverSide = 7,
  kCoverLevel = 8,
  kLight = 9,
  kObscurement = 10,
  kCount = 11,
};
inline constexpr InspectorField kInspectorFields = InspectorField::kCount;

// Владеет окном/рендерером SDL и связывает ввод с командами, локальным
// авторитетным GameServer и ClientView. Режим хоста обслуживает удалённых
// клиентов по TCP; режим клиента работает с удалённым хостом без локального
// сервера.
class App {
 public:
  explicit App(AppOptions options);
  ~App() noexcept;

  App(const App&) = delete;
  auto operator=(const App&) -> App& = delete;

  auto Initialize() -> bool;
  auto Shutdown() -> void;

  auto HandleEvent(SDL_Event event) -> void;
  // Масштаб интерфейса для стола с проектором: логическое разрешение меньше
  // окна, поэтому панели и текст рисуются крупнее.
  auto ApplyUiScale() -> void;
  auto CycleUiScale(int steps) -> void;
  auto Iterate() -> void;

 private:
  auto StartHost() -> bool;
  auto StartClient() -> bool;
  auto StartEditor() -> void;
  auto SaveScene() -> void;
  auto LoadScene() -> void;
  auto ResizeScene(int width, int height) -> void;
  auto CenterCamera() -> void;
  // Камера игрока при входе: свой персонаж, свободный персонаж или зона
  // партии вместо середины большой карты.
  auto CenterCameraOnFocus() -> void;
  auto ApplyEditorBrush(Cell cell) -> void;
  // Штрих кисти: клетки между последовательными положениями курсора.
  auto BeginEditorStroke(EditorPaintMode mode, Cell cell) -> void;
  auto ContinueEditorStroke(Cell cell) -> void;
  auto EndEditorStroke() -> void;
  auto PaintEditorCell(Cell cell, EditorPaintMode mode) -> void;
  auto EraseEditorCell(Cell cell) -> void;
  // Токен под клеткой: по нему ЛКМ не рисует, а ПКМ просит подтверждение.
  [[nodiscard]] auto EditorTokenAt(Cell cell) const -> const Token*;
  [[nodiscard]] auto FindEditorToken(TokenId id) const -> const Token*;
  [[nodiscard]] auto FindEditorToken(TokenId id) -> Token*;
  auto RequestDeleteEditorToken(TokenId id) -> void;
  // Перетаскивание токена ЛКМ: клетка проверяется в core/editor_token.
  auto MoveEditorToken(TokenId id, Cell cell) -> void;
  // Следующий цвет палитры для выбранного токена.
  auto CycleEditorTokenColor() -> void;
  // Переименование выбранного токена прямо в панели редактора.
  auto BeginEditorTokenRename() -> void;
  auto CancelEditorTokenRename() -> void;
  auto CommitEditorTokenRename() -> void;
  auto HandleEditorTokenTextInput(const char* text) -> void;
  auto HandleEditorTokenEditKeyDown(const SDL_KeyboardEvent& key) -> void;
  // Подложка карты в редакторе: M открывает строку, Enter применяет.
  auto BeginEditorBackgroundEdit() -> void;
  auto CancelEditorBackgroundEdit() -> void;
  auto CommitEditorBackgroundEdit() -> void;
  auto HandleEditorBackgroundTextInput(const char* text) -> void;
  auto HandleEditorBackgroundKeyDown(const SDL_KeyboardEvent& key) -> void;
  // Инспектор клетки в редакторе: G открывает свойства под курсором.
  auto OpenEditorInspector() -> void;
  auto CloseEditorInspector() -> void;
  auto ApplyEditorInspector() -> void;
  auto HandleEditorInspectorKeyDown(const SDL_KeyboardEvent& key) -> void;
  [[nodiscard]] auto EditorInspectorFields() const
      -> std::vector<std::pair<std::string, std::string>>;
  auto AddEditorToken(Cell cell, TokenKind kind) -> void;
  // Бестиарий в редакторе: шаблон копируется в токен прямо в сцене.
  auto SpawnEditorBestiary(int index, bool group) -> void;
  auto HandleEditorKeyDown(const SDL_KeyboardEvent& key) -> void;
  auto HandleEditorMouseDown(const SDL_MouseButtonEvent& button) -> void;
  auto HandleEditorMouseUp(const SDL_MouseButtonEvent& button) -> void;
  // Список сцен в редакторе: выбор цели для F5 и новая сцена из карты.
  auto OpenEditorScenes() -> void;
  auto CloseEditorScenes() -> void;
  auto HandleEditorScenesKeyDown(const SDL_KeyboardEvent& key) -> void;
  auto HandleEditorScenesMouseDown(const SDL_MouseButtonEvent& button) -> void;
  auto HandleEditorScenesTextInput(const char* text) -> void;
  auto SwitchEditorScene(std::size_t index) -> void;
  auto CreateEditorScene() -> void;
  // Переименование и удаление карты в панели редактора.
  auto BeginEditorSceneRename() -> void;
  auto CancelEditorSceneRename() -> void;
  auto CommitEditorSceneRename() -> void;
  auto RequestDeleteEditorScene() -> void;
  auto DeleteEditorScene(std::size_t index) -> void;
  auto HandlePanMotion(const SDL_MouseMotionEvent& motion) -> void;
  auto HandleWheel(const SDL_MouseWheelEvent& wheel) -> void;
  auto SubmitMenu() -> void;
  // Возвращает стол в стартовое меню: хост сохраняет кампанию и закрывает
  // порт, клиент разрывает соединение. Мир при этом не сбрасывается.
  auto ExitToMenu() -> void;
  auto HandleMenuKeyDown(const SDL_KeyboardEvent& key) -> void;
  auto HandleMenuTextInput(const char* text) -> void;
  auto HandleMenuMouseDown(const SDL_MouseButtonEvent& button) -> void;
  // Баннер обновления в меню: что показать и что делает кнопка.
  [[nodiscard]] auto MenuUpdateState() const -> MenuUpdate;
  // Диалог несовпадения версий после отказа хоста: тексты и кнопки.
  [[nodiscard]] auto MenuMismatchState() const -> MenuMismatch;
  auto CloseMismatch() -> void;
  // Кнопка диалога: обновление (если помогает) или страница релизов.
  auto HandleMismatchAction() -> void;
  // Версия хоста и последний известный релиз для отказа по протоколу.
  [[nodiscard]] auto MismatchInfo() const -> VersionMismatch;
  // Баннер во время партии: подменять exe можно только из меню.
  [[nodiscard]] auto SessionUpdateText() const -> std::string;
  auto HandleUpdateAction() -> void;
  // Автосейв кампании дописывается, затем exe подменяется и игра выходит.
  auto ApplyUpdate() -> void;
  auto FocusNextMenuField(int direction) -> void;
  auto MenuTextBuffer() -> std::string*;
  auto UpdatePlayersFromRoster(const std::vector<RosterEntry>& roster) -> void;
  // Клик по ростору выбирает персонажа игрока.
  auto SelectPlayerToken(int row) -> void;
  auto PumpHost() -> void;
  auto PumpClient() -> void;
  // После полной пересинхронизации выбранный токен мог исчезнуть: сбрасываем
  // выделение и панели, привязанные к пропавшим токенам.
  auto PruneStaleViewState() -> void;
  // Прокрутка лога: положительное значение — вверх, к старым строкам.
  auto ScrollLog(int lines) -> void;
  // Держит прокрученный лог на тех же строках, когда приходят новые.
  auto SyncLogScroll() -> void;
  // Модальный журнал стола: читает session.log с диска, ищет без учёта
  // регистра и листает прошлые сессии.
  auto OpenLogHistory() -> void;
  auto CloseLogHistory() -> void;
  auto HandleLogHistoryKeyDown(const SDL_KeyboardEvent& key) -> void;
  auto HandleLogHistoryTextInput(const char* text) -> void;
  // Пишет новые строки лога в session.log рядом с .exe.
  auto OpenSessionLog() -> void;
  auto FlushSessionLog() -> void;
  auto ApplyOutcome(PeerId from, const CommandOutcome& outcome) -> void;
  auto HandleMouseDown(const SDL_MouseButtonEvent& button) -> void;
  auto HandleMouseUp(const SDL_MouseButtonEvent& button) -> void;
  auto HandleMouseMotion(const SDL_MouseMotionEvent& motion) -> void;
  auto HandleKeyDown(const SDL_KeyboardEvent& key) -> void;
  auto HandleDmHotkey(SDL_Keycode key) -> void;
  auto SubmitMove(Cell target, bool crawl = false, bool jump = false) -> void;
  auto JumpSelectedToken() -> void;
  // Показывает всему столу клетку «сюда» (Alt+ПКМ или /ping).
  auto SubmitPing(Cell cell) -> void;
  auto SendCommand(const Command& command) -> void;
  auto SubmitChat() -> void;
  // Разобранные поля листа: одна проверка для стола и редактора.
  struct SheetValues {
    int ac = 0;
    int speed = 0;
    int swim = 0;
    int climb = 0;
    int attack = 0;
    int save = 0;
    int initiative = 0;
    int temp_hp = 0;
    int hit_die = 0;
    int con = 0;
    int attack_range = 0;
    // Максимум HP, режим «по костям» и число костей монстра.
    int max_hp = 0;
    bool auto_hp = false;
    int hit_dice_count = 0;
    // Тёмное зрение в метрах: в токене оно хранится в клетках.
    int darkvision = 0;
    // Общий модификатор проверок и маска владений навыками.
    int skill = 0;
    std::uint32_t proficiencies = 0;
    std::string damage;
    std::string notes;
    std::uint32_t resist = 0;
    std::uint32_t vulnerable = 0;
    std::uint32_t immune = 0;
  };

  auto OpenSheet() -> void;
  auto CloseSheet() -> void;
  auto SaveSheet() -> void;
  // Общая отрисовка листа: в игре — токен ClientView, в редакторе — сцены.
  auto DrawActiveSheet() -> void;
  auto LoadSheetFields(const Token& token) -> void;
  [[nodiscard]] auto ReadSheetValues(SheetValues& values) -> bool;
  // Сообщение об ошибке: в редакторе в панель, в игре в лог.
  auto ShowStatus(std::string_view message) -> void;
  auto HandleSheetKeyDown(const SDL_KeyboardEvent& key) -> void;
  auto HandleSheetTextInput(const char* text) -> void;
  auto RollSave() -> void;
  auto RollInitiative() -> void;
  // Атака выбранным токеном по цели под курсором; сервер применяет урон.
  auto AttackTarget(bool advantage, bool disadvantage) -> void;
  [[nodiscard]] auto CanEditToken(const Token& token) const -> bool;
  // Лист своего и любого чужого персонажа открыт на просмотр; прочие токены
  // (монстры, NPC) показывает только DM или их владелец.
  [[nodiscard]] auto CanOpenSheet(const Token& token) const -> bool;
  // DM ведёт стол: правит листы, состояния и прочие данные токенов.
  [[nodiscard]] auto IsDm() const -> bool;
  auto OpenBestiary() -> void;
  auto HandleBestiaryKeyDown(const SDL_KeyboardEvent& key) -> void;
  // Ставит одного монстра или группу из 3–5 под курсором.
  auto SpawnBestiary(int index, bool group) -> void;
  auto OpenConditions() -> void;
  auto CloseConditions() -> void;
  auto ToggleCondition(int index, bool until_removed) -> void;
  // Назначает источник выбранному состоянию по токену под курсором, пустая
  // клетка или сам токен снимают источник.
  auto SetConditionSourceFromCursor() -> void;
  auto HandleConditionsKeyDown(const SDL_KeyboardEvent& key) -> void;
  auto HandleConditionsTextInput(const char* text) -> void;
  auto ApplyConditionDc() -> void;
  // Снять один уровень истощения из черновика состояний.
  auto RemoveExhaustionLevelFromDraft() -> void;
  // Добавить своё состояние из строки ввода.
  auto ApplyConditionCustom(bool until_removed) -> void;
  // Панель действий хода: рывок, отход, уклонение и остальные приёмы 2024.
  auto OpenActions() -> void;
  auto CloseActions() -> void;
  auto HandleActionsKeyDown(const SDL_KeyboardEvent& key) -> void;
  // Состав боя: DM решает, кто ходит в инициативе, а кто ждёт за сценой.
  auto OpenCombat() -> void;
  auto CloseCombat() -> void;
  auto HandleCombatKeyDown(const SDL_KeyboardEvent& key) -> void;
  auto ToggleCombatParticipant() -> void;
  auto ToggleAoeMode() -> void;
  auto ClearAoe() -> void;
  auto SendAoe(const AoeTemplate& aoe) -> void;
  auto UpdateAoeFromCursor() -> void;
  auto HandleAoeKeyDown(const SDL_KeyboardEvent& key) -> bool;
  auto ClaimSelectedToken() -> void;
  auto ReleaseSelectedToken() -> void;
  // Новый ход в бою выбирает своего токена: DM — любого, игрок — своего.
  auto FollowActiveTurn() -> void;
  [[nodiscard]] auto PlayerHasCharacter() const -> bool;
  auto CycleSelectedColor() -> void;
  auto AdjustSelectedHp(int delta) -> void;
  auto AdjustSelectedInitiative(int delta) -> void;
  auto SaveGame() -> void;
  auto LoadGame() -> void;
  // Кампанию пишет только редактор: игра меняет лишь оверлей партии.
  auto SaveCampaign() -> void;
  // Ставит кампанию в фоновую запись; кадр диск не ждёт.
  auto RequestCampaignSave() -> void;
  // Партия: запись оверлея, явная и фоновая.
  auto SaveOverlay() -> bool;
  auto RequestOverlaySave() -> void;
  [[nodiscard]] auto OverlayFilePath() const -> std::filesystem::path;
  // Библиотека кампаний: файлы *.dndcamp рядом с .exe и выбор при старте.
  auto RefreshCampaignLibrary() -> void;
  // Режим загрузки: редактор видит только кампанию, игра — с партией.
  enum class LoadMode : std::uint8_t { kEditor, kGame };
  // Читает выбранный файл в campaign_/scene_/server_/view_; false — не вышло.
  auto LoadSelectedCampaign(LoadMode mode) -> bool;
  // Пустой слот: кампания из scene.dndscene или стартовой карты.
  auto PrepareDefaultCampaign() -> void;
  // Собирает campaign_ из base_campaign_ и overlay_.
  auto ApplyPartyToCampaign() -> void;
  // «Новая партия»: оверлей выбранной кампании начинается с чистого листа.
  auto StartNewParty() -> void;
  // Подтверждение партии от другой версии кампании.
  auto AcceptParty() -> void;
  // «Сбросить сцену к кампании»: убрать запись оверлея для активной сцены.
  auto ResetActiveSceneToCampaign() -> void;
  auto HandleMenuCommand(MenuCommand command) -> void;
  auto SelectCampaign(int delta) -> void;
  // Сохраняет мир хоста в активную сцену кампании и в оверлей партии.
  auto StoreActiveScene() -> void;
  // Изменение мира: партия уйдёт на диск после kAutosaveDelay.
  auto MarkPartyDirty() -> void;
  auto CheckAutosave() -> void;
  // Заменяет мир и пересобирает вид хоста, клиентам уходит ViewUpdated.
  auto ResetWorld(GameSnapshot snapshot) -> void;
  // Восстановление мира без сброса боя (для Ctrl+Z).
  auto RestoreUndoWorld(GameSnapshot snapshot) -> void;
  auto ApplyWorld(GameSnapshot snapshot, bool reset_encounter) -> void;
  auto BroadcastWorld() -> void;
  // Отмена последнего действия DM: храним до 10 предыдущих снапшотов.
  auto PushUndo(GameSnapshot snapshot) -> void;
  auto UndoLastAction() -> void;
  auto ClearUndo() -> void;
  // Удаление токена требует повторного Del в течение 3 секунд.
  auto RequestDeleteSelectedToken() -> void;
  // Пишет строку лога всем за столом (смена сцены и подобное).
  auto Announce(std::string text) -> void;
  // В игре сцены только загружаются: структуру кампании правит редактор.
  auto SwitchScene(std::size_t index) -> void;
  auto OpenScenes() -> void;
  auto CloseScenes() -> void;
  auto HandleScenesKeyDown(const SDL_KeyboardEvent& key) -> void;
  // Клик по сцене выбирает её, двойной — открывает.
  auto HandleScenesMouseDown(const SDL_MouseButtonEvent& button) -> void;
  auto SwitchSceneByName(std::string_view name) -> void;
  // Подложка карты: текстура из файла рядом с .exe и её отрисовка под сеткой.
  auto BackgroundTexture(std::string_view image) -> SDL_Texture*;
  auto DrawBackground(const GridMap& map, const MapBackground& background)
      -> void;
  // Портреты токенов: одна текстура на имя файла, nullptr — файла нет.
  auto PortraitTexture(const std::string& image) -> SDL_Texture*;
  auto LoadFont() -> void;

  [[nodiscard]] auto CellFromPixel(float x, float y) const -> Cell;
  [[nodiscard]] auto TokenAt(Cell cell) const -> TokenId;
  [[nodiscard]] auto FindSelectedToken() const -> const Token*;
  // Текст нижней подсказки: набор клавиш зависит от режима, выделения и роли.
  [[nodiscard]] auto BottomHintText() const -> std::string;

  AppOptions options_;

  SDL_Window* window_ = nullptr;
  SDL_Renderer* renderer_ = nullptr;
  TTF_Font* font_ = nullptr;
  std::unique_ptr<TextRenderer> text_;
  bool ttf_initialized_ = false;

  GameSnapshot scene_;
  Campaign campaign_;
  // Авторский файл кампании: игра его только читает.
  Campaign base_campaign_;
  Overlay overlay_;
  GameServer server_;
  ClientView view_;
  LocalTransport transport_;
  NetServer net_server_;
  NetClient net_client_;
  // Файлы кампаний рядом с .exe (только имена), отсортированы.
  std::vector<std::string> campaign_files_;
  std::size_t campaign_index_ = 0;

  MenuState menu_;
  std::vector<std::string> local_addresses_;
  std::vector<std::string> players_;
  // Владелец токенов для каждой строки ростера; пусто у DM.
  std::vector<std::string> player_owners_;
  bool in_game_ = false;
  bool in_editor_ = false;
  // Esc без активного режима спрашивает подтверждение выхода в меню.
  bool exit_confirm_active_ = false;
  // Сцена, которую редактор сейчас правит и куда пишет F5.
  std::size_t editor_scene_ = 0;
  bool editor_scenes_active_ = false;
  std::size_t editor_scenes_selected_ = 0;
  StatusMessage editor_scenes_status_;
  // Имя карты в панели редактора набирается с нуля; первый TEXT_INPUT — это
  // сама клавиша R.
  bool editor_scene_edit_ = false;
  std::string editor_scene_edit_buffer_;
  bool editor_scene_edit_ignore_text_ = false;
  DeleteConfirm delete_editor_scene_;
  bool client_connecting_ = false;
  // Проверка обновлений работает в фоне; сервис создаётся в Initialize даже
  // при --no-update-check, просто с выключенной проверкой.
  std::unique_ptr<UpdateService> update_service_;
  // «Позже» в баннере: до перезапуска игры больше не показываем.
  bool update_dismissed_ = false;
  // Отказ хоста по протоколу: диалог версий в меню, пока его не закроют.
  std::optional<VersionMismatch> version_mismatch_;

  EditorBrush editor_brush_ = EditorBrush::kWall;
  StatusMessage editor_status_;
  bool editor_panel_visible_ = true;
  EditorPaintMode editor_paint_mode_ = EditorPaintMode::kNone;
  PaintDrag editor_stroke_;
  // ПКМ по токену удаляет его только вторым нажатием в течение 3 с.
  DeleteConfirm delete_editor_token_;
  TokenId editor_drag_token_ = 0;
  bool editor_token_edit_ = false;
  TokenId editor_token_edit_id_ = 0;
  std::string editor_token_edit_buffer_;
  // Первый TEXT_INPUT после R — сама клавиша, её не вводим.
  bool editor_token_edit_ignore_text_ = false;
  // Подложка карты: M открывает строку «файл, off, opacity N, scale N».
  bool editor_background_edit_ = false;
  std::string editor_background_buffer_;
  bool editor_background_ignore_text_ = false;
  // Инспектор клетки: G открывает панель свойств под курсором.
  bool editor_inspector_active_ = false;
  Cell editor_inspector_cell_{};
  int editor_inspector_field_ = 0;
  CellProps editor_inspector_props_{};
  int editor_inspector_cover_side_ = 0;
  CoverLevel editor_inspector_cover_level_ = CoverLevel::kNone;

  bool sheet_active_ = false;
  bool sheet_read_only_ = false;
  TokenId sheet_token_id_ = 0;
  SheetField sheet_field_ = SheetField::kAc;
  std::string sheet_ac_;
  std::string sheet_speed_;
  std::string sheet_swim_;
  std::string sheet_climb_;
  std::string sheet_attack_;
  std::string sheet_save_;
  std::string sheet_initiative_;
  std::string sheet_temp_hp_;
  std::string sheet_hit_die_;
  std::string sheet_con_;
  std::string sheet_range_;
  std::string sheet_darkvision_;
  std::string sheet_max_hp_;
  std::string sheet_auto_hp_;
  std::string sheet_hit_dice_;
  std::string sheet_damage_;
  std::string sheet_resist_;
  std::string sheet_vulnerable_;
  std::string sheet_immune_;
  std::string sheet_notes_;
  std::string sheet_skill_;
  std::string sheet_proficiencies_;

  bool bestiary_active_ = false;
  bool conditions_active_ = false;
  bool conditions_read_only_ = false;
  TokenId conditions_token_id_ = 0;
  std::vector<Condition> conditions_draft_;
  std::size_t conditions_selected_ = 0;
  bool conditions_dc_edit_ = false;
  std::string conditions_dc_buffer_;
  // Ввод своего состояния: T открывает строку, Enter добавляет.
  bool conditions_custom_edit_ = false;
  std::string conditions_custom_buffer_;
  // Первый TEXT_INPUT после T — это сама горячая клавиша, её не вводим.
  bool conditions_custom_ignore_text_ = false;

  bool actions_active_ = false;
  TokenId actions_token_id_ = 0;
  // Модальный состав боя: строка списка всех видимых токенов.
  bool combat_active_ = false;
  std::size_t combat_selected_ = 0;

  bool scenes_active_ = false;
  std::size_t scenes_selected_ = 0;

  bool show_movement_range_ = false;
  bool aoe_mode_ = false;
  bool measuring_ = false;
  std::optional<Cell> measure_start_;
  std::optional<Cell> measure_end_;
  Camera camera_;
  bool panning_ = false;
  float pan_last_x_ = 0.0F;
  float pan_last_y_ = 0.0F;

  bool client_connected_ = false;
  bool client_is_dm_ = false;
  // Игрок уже выбирал персонажа в этой кампании: повторный выбор сервер
  // отклонит, поэтому подсказка «забрать» больше не показывается.
  bool claimed_once_ = false;
  std::chrono::steady_clock::time_point next_reconnect_;
  std::chrono::steady_clock::time_point connect_deadline_;

  ChatInput chat_;

  AutosaveTimer autosave_;
  // Автосейв пишет кампанию в отдельном потоке, чтобы кадр не ждал диск.
  CampaignSaver campaign_saver_;
  // Игра пишет оверлей партии, а не кампанию.
  OverlaySaver overlay_saver_;
  bool campaign_file_loaded_ = false;
  bool party_exists_ = false;
  bool party_error_ = false;
  bool party_mismatch_ = false;
  // Последняя запись партии не дошла до диска: менять кампанию нельзя, пока
  // повторная запись не удастся.
  bool overlay_write_failed_ = false;
  // «Новая партия» требует повторного N.
  DeleteConfirm new_party_confirm_;
  // Правка мира: номер текущей версии и версии, ушедшей в последнюю запись.
  std::uint64_t world_revision_ = 0;
  std::uint64_t saved_revision_ = 0;

  bool help_active_ = false;
  LogScroll log_scroll_{kLogVisibleLines};
  // Журнал стола: строки session.log, отфильтрованные по запросу.
  bool history_active_ = false;
  std::vector<HistoryLine> history_lines_;
  std::vector<std::size_t> history_matches_;
  std::string history_query_;
  LogScroll history_scroll_{kLogHistoryVisibleRows};
  // Индекс в kUiScales: 1.0, 1.25, 1.5, 2.0.
  int ui_scale_index_ = 0;
  // Файл сессии и число строк, которые в него уже попали.
  SessionLog session_log_;
  std::size_t log_written_ = 0;
  // Верхняя граница панели лога в прошлом кадре; 0 — панели не было.
  float log_top_ = 0.0F;
  // Верх панели игроков: по нему считается строка под мышью.
  float players_top_ = 0.0F;

  // Кэш картинки подложки: одна текстура на текущее имя файла.
  SDL_Texture* background_texture_ = nullptr;
  std::string background_texture_name_;
  // Кэш портретов токенов по имени файла.
  std::map<std::string, SDL_Texture*> portrait_textures_;
  // Размер группы монстров; сид не важен, важна неповторяемость.
  std::mt19937 rng_{std::random_device{}()};

  std::deque<GameSnapshot> undo_stack_;
  DeleteConfirm delete_token_;

  TokenId selected_id_ = 0;
  // Активный токен прошлого кадра: выбор переезжает только при смене хода.
  TokenId followed_turn_id_ = 0;
  bool dragging_ = false;
  std::optional<Cell> drag_target_;
  float mouse_x_ = 0.0F;
  float mouse_y_ = 0.0F;
};

}  // namespace dnd
