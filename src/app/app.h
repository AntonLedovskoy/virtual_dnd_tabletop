#pragma once

#include <SDL3/SDL.h>

#include <chrono>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <string_view>
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
#include "core/game_server.h"
#include "core/local_transport.h"
#include "core/log_history.h"
#include "core/log_scroll.h"
#include "core/menu_model.h"
#include "core/session_log.h"
#include "net/net_client.h"
#include "net/net_server.h"
#include "render/editor_renderer.h"
#include "render/menu_renderer.h"
#include "render/ui_renderer.h"

struct TTF_Font;

namespace dnd {

class TextRenderer;

enum class AppMode : std::uint8_t { kHost, kClient };

// Что DM правит прямо в панели сцен.
enum class SceneEditField : std::uint8_t { kNone, kName, kNote };

struct AppOptions {
  AppMode mode = AppMode::kHost;
  std::string connect_host = "127.0.0.1";
  std::uint16_t port = 7777;
  std::string player_name = "player";
  std::string password;
  bool auto_start = false;
  bool start_editor = false;
  // `--no-update-check`: отладка и офлайн. Проверку обновлений читает 7.3.
  bool no_update_check = false;
};

// Владеет окном/рендерером SDL и связывает ввод с командами, локальным
// авторитетным GameServer и ClientView. Режим хоста обслуживает удалённых
// клиентов по TCP; режим клиента работает с удалённым хостом без локального
// сервера.
class App {
 public:
  explicit App(AppOptions options);
  ~App();

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
  auto ApplyEditorBrush(Cell cell) -> void;
  auto AddEditorToken(Cell cell, TokenKind kind) -> void;
  auto HandleEditorKeyDown(const SDL_KeyboardEvent& key) -> void;
  auto HandleEditorMouseDown(const SDL_MouseButtonEvent& button) -> void;
  auto HandleEditorMouseUp(const SDL_MouseButtonEvent& button) -> void;
  // Список сцен в редакторе: выбор цели для F5 и новая сцена из карты.
  auto OpenEditorScenes() -> void;
  auto CloseEditorScenes() -> void;
  auto HandleEditorScenesKeyDown(const SDL_KeyboardEvent& key) -> void;
  auto HandleEditorScenesMouseDown(const SDL_MouseButtonEvent& button) -> void;
  auto SwitchEditorScene(std::size_t index) -> void;
  auto CreateEditorScene() -> void;
  auto HandlePanMotion(const SDL_MouseMotionEvent& motion) -> void;
  auto HandleWheel(const SDL_MouseWheelEvent& wheel) -> void;
  auto SubmitMenu() -> void;
  // Возвращает стол в стартовое меню: хост сохраняет кампанию и закрывает
  // порт, клиент разрывает соединение. Мир при этом не сбрасывается.
  auto ExitToMenu() -> void;
  auto HandleMenuKeyDown(const SDL_KeyboardEvent& key) -> void;
  auto HandleMenuTextInput(const char* text) -> void;
  auto HandleMenuMouseDown(const SDL_MouseButtonEvent& button) -> void;
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
  auto SubmitMove(Cell target) -> void;
  // Показывает всему столу клетку «сюда» (Alt+ПКМ или /ping).
  auto SubmitPing(Cell cell) -> void;
  auto SendCommand(const Command& command) -> void;
  auto SubmitChat() -> void;
  auto OpenSheet() -> void;
  auto CloseSheet() -> void;
  auto SaveSheet() -> void;
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
  auto HandleConditionsKeyDown(const SDL_KeyboardEvent& key) -> void;
  auto HandleConditionsTextInput(const char* text) -> void;
  auto ApplyConditionDc() -> void;
  auto ToggleAoeMode() -> void;
  auto ClearAoe() -> void;
  auto SendAoe(const AoeTemplate& aoe) -> void;
  auto UpdateAoeFromCursor() -> void;
  auto HandleAoeKeyDown(const SDL_KeyboardEvent& key) -> bool;
  auto ClaimSelectedToken() -> void;
  auto ReleaseSelectedToken() -> void;
  [[nodiscard]] auto PlayerHasCharacter() const -> bool;
  auto CycleSelectedColor() -> void;
  auto AdjustSelectedHp(int delta) -> void;
  auto AdjustSelectedInitiative(int delta) -> void;
  auto SaveGame() -> void;
  auto LoadGame() -> void;
  auto SaveCampaign() -> void;
  // Ставит кампанию в фоновую запись; кадр диск не ждёт.
  auto RequestCampaignSave() -> void;
  // Библиотека кампаний: файлы *.dndcamp рядом с .exe и выбор при старте.
  auto RefreshCampaignLibrary() -> void;
  // Читает выбранный файл в campaign_/scene_/server_/view_; false — не вышло.
  auto LoadSelectedCampaign() -> bool;
  auto SelectCampaign(int delta) -> void;
  // Сохраняет мир хоста в активную сцену кампании.
  auto StoreActiveScene() -> void;
  // Изменение мира: кампания уйдёт на диск после kAutosaveDelay.
  auto MarkCampaignDirty() -> void;
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
  // Удаление токена и сцены требует повторного Del в течение 3 секунд.
  auto RequestDeleteSelectedToken() -> void;
  auto RequestDeleteActiveScene() -> void;
  // Пишет строку лога всем за столом (смена сцены и подобное).
  auto Announce(std::string text) -> void;
  auto SwitchScene(std::size_t index) -> void;
  auto CreateScene() -> void;
  auto DeleteActiveScene() -> void;
  auto OpenScenes() -> void;
  auto CloseScenes() -> void;
  auto HandleScenesKeyDown(const SDL_KeyboardEvent& key) -> void;
  // Клик по сцене выбирает её, двойной — открывает.
  auto HandleScenesMouseDown(const SDL_MouseButtonEvent& button) -> void;
  auto HandleScenesTextInput(const char* text) -> void;
  auto SwitchSceneByName(std::string_view name) -> void;
  // Переименовывает сцену по индексу; false, если имя пустое или занято.
  auto RenameSceneAt(std::size_t index, std::string name) -> bool;
  // Разбирает хвост команды «/scene rename СТАРОЕ НОВОЕ».
  auto RenameSceneByCommand(std::string_view args) -> void;
  auto BeginSceneEdit(SceneEditField field) -> void;
  auto CancelSceneEdit() -> void;
  auto CommitSceneEdit() -> void;
  // Подложка карты: текстура из файла рядом с .exe и её отрисовка под сеткой.
  auto BackgroundTexture(std::string_view image) -> SDL_Texture*;
  auto DrawBackground(const GridMap& map, const MapBackground& background)
      -> void;
  // Портреты токенов: одна текстура на имя файла, nullptr — файла нет.
  auto PortraitTexture(const std::string& image) -> SDL_Texture*;
  // Ставит подложку по разобранной команде «/map ...».
  auto SubmitMapCommand(const ChatCommand& command) -> void;
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
  std::string editor_scenes_status_;
  bool client_connecting_ = false;

  EditorBrush editor_brush_ = EditorBrush::kWall;
  std::string editor_status_;
  bool editor_panel_visible_ = true;

  bool sheet_active_ = false;
  bool sheet_read_only_ = false;
  TokenId sheet_token_id_ = 0;
  SheetField sheet_field_ = SheetField::kAc;
  std::string sheet_ac_;
  std::string sheet_speed_;
  std::string sheet_attack_;
  std::string sheet_save_;
  std::string sheet_initiative_;
  std::string sheet_temp_hp_;
  std::string sheet_hit_die_;
  std::string sheet_con_;
  std::string sheet_damage_;
  std::string sheet_notes_;

  bool bestiary_active_ = false;
  bool conditions_active_ = false;
  bool conditions_read_only_ = false;
  TokenId conditions_token_id_ = 0;
  std::vector<Condition> conditions_draft_;
  std::size_t conditions_selected_ = 0;
  bool conditions_dc_edit_ = false;
  std::string conditions_dc_buffer_;

  bool scenes_active_ = false;
  std::size_t scenes_selected_ = 0;
  SceneEditField scenes_edit_ = SceneEditField::kNone;
  std::string scenes_edit_buffer_;
  std::string scenes_status_;
  // Первый TEXT_INPUT после R/E — это сама горячая клавиша, её не вводим.
  bool scenes_edit_ignore_text_ = false;

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
  DeleteConfirm delete_scene_;

  TokenId selected_id_ = 0;
  bool dragging_ = false;
  std::optional<Cell> drag_target_;
  float mouse_x_ = 0.0F;
  float mouse_y_ = 0.0F;
};

}  // namespace dnd
