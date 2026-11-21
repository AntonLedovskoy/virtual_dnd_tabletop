#include "app/app.h"

#include <SDL3_image/SDL_image.h>
#include <SDL3_ttf/SDL_ttf.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <ranges>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

#include "app/strings.h"
#include "core/bestiary.h"
#include "core/clock.h"
#include "core/conditions.h"
#include "core/dice.h"
#include "core/navigation.h"
#include "core/player_name.h"
#include "core/scene_io.h"
#include "core/units.h"
#include "core/utf8.h"
#include "net/tcp_socket.h"
#include "render/grid_renderer.h"
#include "render/menu_renderer.h"
#include "render/text_renderer.h"
#include "render/ui_renderer.h"

namespace dnd {
namespace {

constexpr int kWindowWidth = 1280;
constexpr int kWindowHeight = 720;
constexpr int kMapWidth = 24;
constexpr int kMapHeight = 12;
constexpr int kSightRadius = 6;
constexpr float kFontSize = 16.0F;
// Шаги масштаба интерфейса: 100%, 125%, 150%, 200%.
constexpr std::array<float, 4> kUiScales{1.0F, 1.25F, 1.5F, 2.0F};
constexpr std::chrono::seconds kReconnectDelay{2};
constexpr std::size_t kUndoDepth = 10;

constexpr TokenId kHeroId = 1;
constexpr TokenId kGoblinId = 2;
constexpr TokenId kAllyId = 3;

constexpr std::array<std::uint32_t, 6> kTokenPalette{{
    0xC85046U,
    0x46A864U,
    0x507AC8U,
    0xC8A83CU,
    0x9C6CC8U,
    0x46B8B8U,
}};
constexpr std::size_t kAutoTokenColorCount = 4;
// Предел подписи броска с именем токена: совпадает с kMaxRollLabel
// в game_server.cpp.
constexpr std::size_t kMaxLabelBytes = 64;

auto BuildSnapshot() -> GameSnapshot {
  GridMap map(kMapWidth, kMapHeight);
  // Вертикальная стена с одним проходом и горизонтальная стена с разрывом.
  for (int y = 0; y <= 5; ++y) {
    if (y != 3) {
      map.SetBlocked(Cell{.x = 7, .y = y}, true);
    }
  }
  for (int x = 8; x <= 16; ++x) {
    if (x != 12) {
      map.SetBlocked(Cell{.x = x, .y = 6}, true);
    }
  }

  std::vector<Token> tokens{
      Token{.id = kHeroId,
            .name = "Герой",
            .kind = TokenKind::kPlayer,
            .cell = Cell{.x = 2, .y = 2}},
      Token{.id = kGoblinId,
            .name = "Гоблин",
            .owner = "dm",
            .kind = TokenKind::kMonster,
            .cell = Cell{.x = 10, .y = 4}},
      Token{.id = kAllyId,
            .name = "Союзник",
            .kind = TokenKind::kPlayer,
            .cell = Cell{.x = 5, .y = 8}},
  };

  return GameSnapshot{
      .map = std::move(map),
      .tokens = std::move(tokens),
      .next_token_id = kAllyId + 1,
  };
}

auto SavePath() -> std::filesystem::path {
  if (const char* base = SDL_GetBasePath()) {
    return std::filesystem::path(base) / "savegame.bin";
  }
  return {"savegame.bin"};
}

auto ScenePath() -> std::filesystem::path {
  if (const char* base = SDL_GetBasePath()) {
    return std::filesystem::path(base) / "scene.dndscene";
  }
  return {"scene.dndscene"};
}

auto SessionLogPath() -> std::filesystem::path {
  if (const char* base = SDL_GetBasePath()) {
    return std::filesystem::path(base) / "session.log";
  }
  return {"session.log"};
}

// Ресурсы (подложка, портреты) ищутся рядом с .exe: сцена хранит только имя.
auto BaseDirectory() -> std::filesystem::path {
  if (const char* base = SDL_GetBasePath()) {
    return {base};
  }
  return {};
}

auto AssetPath(std::string_view name) -> std::filesystem::path {
  return BaseDirectory() / std::string(name);
}

auto ParseNumber(std::string_view text, int& value) -> bool {
  if (text.empty()) {
    return false;
  }
  int parsed = 0;
  const auto result =
      std::from_chars(text.data(), text.data() + text.size(), parsed);
  if (result.ec != std::errc{} || result.ptr != text.data() + text.size() ||
      parsed <= 0) {
    return false;
  }
  value = parsed;
  return true;
}

auto ParseSignedNumber(std::string_view text, int& value) -> bool {
  if (text.empty()) {
    return false;
  }
  int parsed = 0;
  const auto result =
      std::from_chars(text.data(), text.data() + text.size(), parsed);
  if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) {
    return false;
  }
  value = parsed;
  return true;
}

// Подсказка для команды, которую не удалось разобрать.
auto ChatUsageText(ChatCommandError error) -> std::string_view {
  switch (error) {
    case ChatCommandError::kBadRoll:
      return strings::kRollUsage;
    case ChatCommandError::kBadKick:
      return strings::kKickUsage;
    case ChatCommandError::kBadRename:
      return strings::kRenameUsage;
    case ChatCommandError::kBadMaxHp:
      return strings::kMaxHpUsage;
    case ChatCommandError::kBadDamage:
      return strings::kDamageUsage;
    case ChatCommandError::kBadAoe:
      return strings::kAoeUsage;
    case ChatCommandError::kBadPortrait:
      return strings::kPortraitUsage;
    case ChatCommandError::kBadMap:
      return strings::kMapUsage;
    case ChatCommandError::kBadReveal:
      return strings::kRevealUsage;
    case ChatCommandError::kBadXp:
      return strings::kXpUsage;
    case ChatCommandError::kNone:
      break;
  }
  return {};
}

// Добавляет цифры (и минус, если он разрешён) из text в buffer, пропуская
// всё остальное и не выпуская значение за пределы minimum..maximum.
auto AppendBoundedNumber(std::string& buffer, const char* text, int minimum,
                         int maximum, bool allow_negative) -> void {
  if (text == nullptr) {
    return;
  }
  for (const char* it = text; *it != '\0'; ++it) {
    const bool digit = *it >= '0' && *it <= '9';
    const bool sign = allow_negative && *it == '-' && buffer.empty();
    if (!digit && !sign) {
      continue;
    }
    std::string candidate = buffer;
    candidate.push_back(*it);
    if (candidate == "-") {
      buffer = std::move(candidate);
      continue;
    }
    int value = 0;
    if (ParseSignedNumber(candidate, value) && value >= minimum &&
        value <= maximum) {
      buffer = std::move(candidate);
    }
  }
}

// Подпись броска с именем того, кто бросает: в общем логе видно, к кому
// относится бросок.
auto RollLabel(std::string_view kind, std::string_view token_name)
    -> std::string {
  const std::string prefix = std::string(kind) + " (";
  std::string name(token_name);
  if (prefix.size() + name.size() + 1 > kMaxLabelBytes) {
    TruncateUtf8(name, kMaxLabelBytes - prefix.size() - 1);
  }
  return prefix + name + ")";
}

auto FormatBonus(int bonus) -> std::string {
  if (bonus > 0) {
    return "+" + std::to_string(bonus);
  }
  if (bonus < 0) {
    return std::to_string(bonus);
  }
  return {};
}

auto TrimText(std::string_view text) -> std::string_view {
  while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
    text.remove_prefix(1);
  }
  while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) {
    text.remove_suffix(1);
  }
  return text;
}

auto PointInside(const SDL_FRect& rect, float x, float y) -> bool {
  return x >= rect.x && y >= rect.y && x < rect.x + rect.w &&
         y < rect.y + rect.h;
}

}  // namespace

App::App(AppOptions options)
    : options_(std::move(options)),
      scene_(BuildSnapshot()),
      server_(scene_, kSightRadius) {
  // Хост — это DM: весь мир и никакого тумана.
  view_.Reset(server_.DmSnapshot(), server_.FullFogState());
  menu_.address = options_.connect_host;
  menu_.port = std::to_string(options_.port);
  menu_.name = options_.player_name;
  menu_.password = options_.password;
  menu_.tab =
      options_.mode == AppMode::kClient ? MenuTab::kJoin : MenuTab::kHost;
}

App::~App() { Shutdown(); }

auto App::Initialize() -> bool {
  if (!SDL_Init(SDL_INIT_VIDEO)) {
    SDL_Log("SDL_Init failed: %s", SDL_GetError());
    return false;
  }
  window_ = SDL_CreateWindow("dnd", kWindowWidth, kWindowHeight,
                             SDL_WINDOW_RESIZABLE);
  if (window_ == nullptr) {
    SDL_Log("SDL_CreateWindow failed: %s", SDL_GetError());
    return false;
  }
  renderer_ = SDL_CreateRenderer(window_, nullptr);
  if (renderer_ == nullptr) {
    SDL_Log("SDL_CreateRenderer failed: %s", SDL_GetError());
    return false;
  }
  SDL_StartTextInput(window_);
  // Итерации по vsync; безопасно, если драйвер не поддерживает vsync.
  SDL_SetRenderVSync(renderer_, 1);
  ApplyUiScale();

  LoadFont();
  text_ = std::make_unique<TextRenderer>(renderer_, font_);
  if (!text_->Valid()) {
    SDL_Log("text renderer initialization failed: %s", SDL_GetError());
  }
  local_addresses_ = LocalIpv4Addresses();
  OpenSessionLog();

  RefreshCampaignLibrary();
  if (!LoadSelectedCampaign()) {
    // Файла ещё нет: начинаем с отдельной сцены или со стартовой карты.
    const std::optional<GameSnapshot> loaded_scene = ReadSnapshot(ScenePath());
    if (loaded_scene.has_value()) {
      scene_ = *loaded_scene;
    }
    campaign_ = MakeCampaign(scene_, "Сцена 1");
    // Кампания — источник стартового мира; бой в новой сессии не продолжается.
    scene_ = campaign_.scenes[campaign_.active].snapshot;
    scene_.encounter = Encounter{};
    server_.Reset(scene_);
    view_.Reset(server_.DmSnapshot(), server_.FullFogState());
  }

  if (options_.start_editor) {
    StartEditor();
  } else if (options_.auto_start) {
    if (options_.mode == AppMode::kClient) {
      (void)StartClient();
    } else {
      (void)StartHost();
    }
  }
  return true;
}

auto App::Shutdown() -> void {
  session_log_.Close();
  if (background_texture_ != nullptr) {
    SDL_DestroyTexture(background_texture_);
    background_texture_ = nullptr;
  }
  for (const auto& [name, texture] : portrait_textures_) {
    if (texture != nullptr) {
      SDL_DestroyTexture(texture);
    }
  }
  portrait_textures_.clear();
  text_.reset();
  if (font_ != nullptr) {
    TTF_CloseFont(font_);
    font_ = nullptr;
  }
  if (ttf_initialized_) {
    TTF_Quit();
    ttf_initialized_ = false;
  }
  if (renderer_ != nullptr) {
    SDL_DestroyRenderer(renderer_);
    renderer_ = nullptr;
  }
  if (window_ != nullptr) {
    SDL_DestroyWindow(window_);
    window_ = nullptr;
  }
}

auto App::StartHost() -> bool {
  std::uint16_t port = 0;
  if (!ParsePort(menu_.port, port)) {
    menu_.status = "Некорректный порт";
    if (in_editor_) {
      editor_status_ = menu_.status;
    }
    return false;
  }
  if (!net_server_.Start(port, menu_.password)) {
    menu_.status = "Не удалось открыть порт " + std::to_string(port);
    if (in_editor_) {
      editor_status_ = menu_.status;
    }
    return false;
  }

  options_.mode = AppMode::kHost;
  options_.port = port;
  // Из редактора хостится отредактированная сцена, из меню — выбранная
  // кампания с диска.
  if (!in_editor_) {
    (void)LoadSelectedCampaign();
    scene_ = campaign_.scenes[campaign_.active].snapshot;
  }
  GameSnapshot start = scene_;
  start.encounter = Encounter{};
  scene_ = start;
  server_.Reset(std::move(start));
  ClearUndo();
  delete_token_.Clear();
  delete_scene_.Clear();
  in_game_ = true;
  in_editor_ = false;
  client_connecting_ = false;
  client_connected_ = false;
  menu_.busy = false;
  menu_.status.clear();
  selected_id_ = 0;
  dragging_ = false;
  drag_target_.reset();
  chat_.Close();
  help_active_ = false;
  log_scroll_.ToNewest();
  log_top_ = 0.0F;
  players_.clear();
  players_.emplace_back("DM (хост)");
  player_owners_.clear();
  player_owners_.emplace_back();
  local_addresses_ = LocalIpv4Addresses();
  view_.Reset(server_.DmSnapshot(), server_.FullFogState());
  CenterCamera();
  StoreActiveScene();
  SaveCampaign();
  SDL_Log("hosting on port %u", static_cast<unsigned>(port));
  return true;
}

auto App::StartClient() -> bool {
  std::uint16_t port = 0;
  if (!ParsePort(menu_.port, port)) {
    menu_.status = "Некорректный порт";
    return false;
  }
  if (menu_.name.empty()) {
    menu_.status = "Введите имя игрока";
    return false;
  }

  const HostAddress address = SplitHostPort(menu_.address);
  const std::string host = address.host;
  if (address.has_port) {
    port = address.port;
  }
  if (host.empty()) {
    menu_.status = "Введите адрес хоста";
    return false;
  }

  if (!net_client_.Connect(host, port, menu_.name, menu_.password)) {
    menu_.status = "Не удалось начать подключение";
    menu_.busy = false;
    client_connecting_ = false;
    return false;
  }

  options_.mode = AppMode::kClient;
  options_.connect_host = host;
  options_.port = port;
  options_.player_name = menu_.name;
  menu_.busy = true;
  menu_.status = "Подключение...";
  client_connecting_ = true;
  client_connected_ = false;
  in_game_ = false;
  help_active_ = false;
  log_scroll_.ToNewest();
  log_top_ = 0.0F;
  players_.clear();
  const auto now = std::chrono::steady_clock::now();
  next_reconnect_ = now + kReconnectDelay;
  connect_deadline_ = now + std::chrono::seconds(10);
  SDL_Log("connecting to %s:%u as '%s'", host.c_str(),
          static_cast<unsigned>(port), menu_.name.c_str());
  return true;
}

auto App::StartEditor() -> void {
  in_editor_ = true;
  in_game_ = false;
  client_connecting_ = false;
  menu_.status.clear();
  editor_status_.clear();
  editor_panel_visible_ = true;
  // Редактор начинает с активной сцены кампании; F5 пишет именно её.
  if (campaign_.active < campaign_.scenes.size()) {
    editor_scene_ = campaign_.active;
    scene_ = campaign_.scenes[editor_scene_].snapshot;
    scene_.encounter = Encounter{};
  } else {
    editor_scene_ = 0;
  }
  editor_scenes_active_ = false;
  editor_scenes_selected_ = editor_scene_;
  editor_scenes_status_.clear();
  ClearUndo();
  selected_id_ = 0;
  dragging_ = false;
  drag_target_.reset();
  panning_ = false;
  chat_.Close();
  help_active_ = false;
  log_scroll_.ToNewest();
  log_top_ = 0.0F;
  CenterCamera();
}

auto App::SubmitMenu() -> void {
  if (menu_.busy) {
    return;
  }
  if (menu_.tab == MenuTab::kHost) {
    (void)StartHost();
  } else if (menu_.tab == MenuTab::kJoin) {
    (void)StartClient();
  } else {
    StartEditor();
  }
}

auto App::SaveScene() -> void {
  if (editor_scene_ >= campaign_.scenes.size()) {
    editor_status_ = "Сцена не выбрана";
    return;
  }
  // Отдельный файл сцены — шаблон карты: туман прошлой партии в него не пишем.
  GameSnapshot stored = scene_;
  stored.fog.clear();
  if (!WriteSnapshot(stored, ScenePath())) {
    editor_status_ = "Не удалось сохранить сцену";
    return;
  }
  // F5 пишет ту сцену, которую выбрал DM в панели редактора.
  campaign_.scenes[editor_scene_].snapshot = scene_;
  campaign_.active = editor_scene_;
  SaveCampaign();
  editor_status_ = std::string(strings::kEditorSceneSaved) + ": " +
                   campaign_.scenes[editor_scene_].name;
}

auto App::LoadScene() -> void {
  const std::optional<GameSnapshot> loaded = ReadSnapshot(ScenePath());
  if (!loaded.has_value()) {
    editor_status_ = std::string(strings::kEditorSceneMissing);
    return;
  }
  scene_ = *loaded;
  scene_.encounter = Encounter{};
  selected_id_ = 0;
  dragging_ = false;
  drag_target_.reset();
  editor_status_ = std::string(strings::kEditorSceneLoaded);
  CenterCamera();
}

auto App::ResizeScene(int width, int height) -> void {
  width = std::clamp(width, 4, 100);
  height = std::clamp(height, 4, 100);
  if (width == scene_.map.Width() && height == scene_.map.Height()) {
    return;
  }
  GridMap map(width, height);
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      const Cell cell{.x = x, .y = y};
      if (scene_.map.Contains(cell) && scene_.map.IsBlocked(cell)) {
        map.SetBlocked(cell, true);
      }
    }
  }
  scene_.map = std::move(map);
  std::erase_if(scene_.tokens, [this](const Token& token) -> bool {
    return !scene_.map.Contains(token.cell);
  });
  scene_.encounter = Encounter{};
  selected_id_ = 0;
  dragging_ = false;
  drag_target_.reset();
  editor_status_ =
      "Размер карты: " + std::to_string(width) + " x " + std::to_string(height);
}

auto App::CenterCamera() -> void {
  int window_width = 0;
  int window_height = 0;
  if (!RenderSize(renderer_, window_width, window_height)) {
    return;
  }
  const GridMap& map = in_editor_ ? scene_.map : view_.Map();
  camera_ = CenteredCamera(static_cast<float>(window_width),
                           static_cast<float>(window_height), map.Width(),
                           map.Height(), camera_.cell_size);
}

auto App::AddEditorToken(Cell cell, TokenKind kind) -> void {
  if (!scene_.map.Contains(cell) || scene_.map.IsBlocked(cell)) {
    editor_status_ = "Нельзя поставить токен в эту клетку";
    return;
  }
  for (const Token& token : scene_.tokens) {
    if (token.cell == cell) {
      editor_status_ = "Клетка уже занята токеном";
      return;
    }
  }
  const bool player = kind == TokenKind::kPlayer;
  scene_.tokens.push_back(Token{
      .id = scene_.next_token_id++,
      .name = player ? "Персонаж" : "Монстр",
      .owner = player ? "" : "dm",
      .hp = 10,
      .max_hp = 10,
      .kind = kind,
      .cell = cell,
  });
}

auto App::ApplyEditorBrush(Cell cell) -> void {
  if (!scene_.map.Contains(cell)) {
    return;
  }
  switch (editor_brush_) {
    case EditorBrush::kWall:
      scene_.map.SetBlocked(cell, true);
      std::erase_if(scene_.tokens, [cell](const Token& token) -> bool {
        return token.cell == cell;
      });
      break;
    case EditorBrush::kFloor:
      scene_.map.SetBlocked(cell, false);
      break;
    case EditorBrush::kPlayer:
      AddEditorToken(cell, TokenKind::kPlayer);
      break;
    case EditorBrush::kMonster:
      AddEditorToken(cell, TokenKind::kMonster);
      break;
    case EditorBrush::kErase: {
      const auto before = scene_.tokens.size();
      std::erase_if(scene_.tokens, [cell](const Token& token) -> bool {
        return token.cell == cell;
      });
      if (scene_.tokens.size() == before) {
        scene_.map.SetBlocked(cell, false);
      }
      break;
    }
  }
  scene_.encounter = Encounter{};
}

auto App::HandleEditorKeyDown(const SDL_KeyboardEvent& key) -> void {
  switch (key.key) {
    case SDLK_ESCAPE:
      in_editor_ = false;
      menu_.tab = MenuTab::kHost;
      menu_.status.clear();
      RefreshCampaignLibrary();
      return;
    case SDLK_TAB:
      OpenEditorScenes();
      return;
    case SDLK_F2:
      editor_panel_visible_ = !editor_panel_visible_;
      return;
    case SDLK_F5:
      SaveScene();
      return;
    case SDLK_F9:
      LoadScene();
      return;
    case SDLK_RETURN:
    case SDLK_KP_ENTER:
      (void)StartHost();
      return;
    case SDLK_LEFTBRACKET:
      ResizeScene(scene_.map.Width() - 1, scene_.map.Height());
      return;
    case SDLK_RIGHTBRACKET:
      ResizeScene(scene_.map.Width() + 1, scene_.map.Height());
      return;
    case SDLK_MINUS:
      ResizeScene(scene_.map.Width(), scene_.map.Height() - 1);
      return;
    case SDLK_EQUALS:
      ResizeScene(scene_.map.Width(), scene_.map.Height() + 1);
      return;
    case SDLK_1:
      editor_brush_ = EditorBrush::kWall;
      return;
    case SDLK_2:
      editor_brush_ = EditorBrush::kFloor;
      return;
    case SDLK_3:
      editor_brush_ = EditorBrush::kPlayer;
      return;
    case SDLK_4:
      editor_brush_ = EditorBrush::kMonster;
      return;
    case SDLK_5:
      editor_brush_ = EditorBrush::kErase;
      return;
    case SDLK_HOME:
      CenterCamera();
      return;
    default:
      break;
  }
}

auto App::OpenEditorScenes() -> void {
  editor_scenes_active_ = true;
  editor_scenes_selected_ = editor_scene_;
  editor_scenes_status_.clear();
}

auto App::CloseEditorScenes() -> void {
  editor_scenes_active_ = false;
  editor_scenes_status_.clear();
}

auto App::HandleEditorScenesMouseDown(const SDL_MouseButtonEvent& button)
    -> void {
  if (button.button != SDL_BUTTON_LEFT) {
    return;
  }
  const std::size_t index =
      SceneRowAt(renderer_, text_.get(), campaign_.scenes,
                 editor_scenes_selected_, button.x, button.y);
  if (index == kNoScene) {
    return;
  }
  editor_scenes_selected_ = index;
  editor_scenes_status_.clear();
  if (button.clicks >= 2) {
    SwitchEditorScene(index);
  }
}

auto App::HandleEditorScenesKeyDown(const SDL_KeyboardEvent& key) -> void {
  switch (key.key) {
    case SDLK_ESCAPE:
    case SDLK_TAB:
      CloseEditorScenes();
      return;
    case SDLK_UP:
      editor_scenes_selected_ =
          StepIndex(editor_scenes_selected_, -1, campaign_.scenes.size());
      return;
    case SDLK_DOWN:
      editor_scenes_selected_ =
          StepIndex(editor_scenes_selected_, 1, campaign_.scenes.size());
      return;
    case SDLK_RETURN:
    case SDLK_KP_ENTER:
      SwitchEditorScene(editor_scenes_selected_);
      return;
    case SDLK_N:
      CreateEditorScene();
      return;
    case SDLK_F5:
      SaveScene();
      return;
    default:
      break;
  }
  if (key.key >= SDLK_1 && key.key <= SDLK_9) {
    const std::optional<std::size_t> index = IndexForDigit(
        static_cast<int>(key.key - SDLK_1) + 1, campaign_.scenes.size());
    if (index.has_value()) {
      editor_scenes_selected_ = *index;
      SwitchEditorScene(*index);
    }
  }
}

auto App::SwitchEditorScene(std::size_t index) -> void {
  if (index >= campaign_.scenes.size()) {
    return;
  }
  if (index != editor_scene_ && editor_scene_ < campaign_.scenes.size()) {
    // Правки остаются в памяти кампании и уйдут на диск при F5 или старте.
    campaign_.scenes[editor_scene_].snapshot = scene_;
  }
  editor_scene_ = index;
  campaign_.active = index;
  editor_scenes_selected_ = index;
  scene_ = campaign_.scenes[index].snapshot;
  scene_.encounter = Encounter{};
  selected_id_ = 0;
  dragging_ = false;
  drag_target_.reset();
  panning_ = false;
  CenterCamera();
  editor_status_ =
      std::string(strings::kEditorSceneOpened) + campaign_.scenes[index].name;
  CloseEditorScenes();
}

auto App::CreateEditorScene() -> void {
  editor_scenes_status_.clear();
  if (campaign_.scenes.size() >= kMaxCampaignScenes) {
    editor_scenes_status_ = "Достигнут предел числа сцен";
    return;
  }
  if (editor_scene_ < campaign_.scenes.size()) {
    campaign_.scenes[editor_scene_].snapshot = scene_;
  }
  std::string name = NextSceneName(campaign_);
  if (!IsSceneNameAvailable(campaign_, name)) {
    editor_scenes_status_ = std::string(strings::kSceneNameTaken);
    return;
  }
  Scene scene{.name = std::move(name), .snapshot = scene_};
  scene.snapshot.encounter = Encounter{};
  campaign_.scenes.push_back(std::move(scene));
  campaign_.active = campaign_.scenes.size() - 1;
  editor_scene_ = campaign_.active;
  editor_scenes_selected_ = editor_scene_;
  SaveCampaign();
  editor_status_ = std::string(strings::kEditorSceneCreated) +
                   campaign_.scenes[editor_scene_].name;
}

auto App::HandleEditorMouseDown(const SDL_MouseButtonEvent& button) -> void {
  const SDL_FRect panel = EditorPanelBounds();
  if (editor_panel_visible_ && PointInside(panel, button.x, button.y)) {
    return;
  }
  if (button.button == SDL_BUTTON_MIDDLE) {
    panning_ = true;
    pan_last_x_ = button.x;
    pan_last_y_ = button.y;
    return;
  }
  const Cell cell = CellFromPixel(button.x, button.y);
  if (button.button == SDL_BUTTON_LEFT) {
    ApplyEditorBrush(cell);
  } else if (button.button == SDL_BUTTON_RIGHT) {
    const auto before = scene_.tokens.size();
    std::erase_if(scene_.tokens, [cell](const Token& token) -> bool {
      return token.cell == cell;
    });
    if (scene_.tokens.size() == before) {
      scene_.map.SetBlocked(cell, false);
    }
    scene_.encounter = Encounter{};
  }
}

auto App::HandleEditorMouseUp(const SDL_MouseButtonEvent& button) -> void {
  if (button.button == SDL_BUTTON_MIDDLE) {
    panning_ = false;
  }
}

auto App::HandlePanMotion(const SDL_MouseMotionEvent& motion) -> void {
  if (!panning_) {
    return;
  }
  PanCamera(camera_, static_cast<float>(motion.x) - pan_last_x_,
            static_cast<float>(motion.y) - pan_last_y_);
  pan_last_x_ = static_cast<float>(motion.x);
  pan_last_y_ = static_cast<float>(motion.y);
}

auto App::HandleWheel(const SDL_MouseWheelEvent& wheel) -> void {
  if (wheel.y == 0.0F) {
    return;
  }
  // Колесо над логом листает историю, а не приближает карту.
  if (in_game_ && log_top_ > 0.0F && wheel.mouse_y >= log_top_) {
    ScrollLog(static_cast<int>(wheel.y * 3.0F));
    return;
  }
  ZoomCamera(camera_, wheel.mouse_x, wheel.mouse_y, wheel.y);
}

auto App::MenuTextBuffer() -> std::string* { return ActiveMenuText(menu_); }

auto App::FocusNextMenuField(int direction) -> void {
  menu_.active_field = NextMenuField(menu_.tab, menu_.active_field, direction);
}

auto App::HandleMenuTextInput(const char* text) -> void {
  std::string* buffer = MenuTextBuffer();
  if (buffer == nullptr || text == nullptr) {
    return;
  }
  AppendUtf8(*buffer, text, MenuFieldLimit(menu_.active_field));
}

auto App::HandleMenuKeyDown(const SDL_KeyboardEvent& key) -> void {
  // Стрелки и цифровой блок одинаково листают библиотеку кампаний.
  const bool up = key.key == SDLK_UP || key.key == SDLK_KP_8 ||
                  key.scancode == SDL_SCANCODE_UP;
  const bool down = key.key == SDLK_DOWN || key.key == SDLK_KP_2 ||
                    key.scancode == SDL_SCANCODE_DOWN;
  if (menu_.tab == MenuTab::kHost && (up || down)) {
    SelectCampaign(down ? 1 : -1);
    menu_.active_field = MenuField::kNone;
    return;
  }
  if (key.key == SDLK_LEFT || key.key == SDLK_RIGHT) {
    menu_.tab = NextMenuTab(menu_.tab, key.key == SDLK_RIGHT ? 1 : -1);
    menu_.active_field = MenuField::kNone;
    menu_.status.clear();
    return;
  }
  if (key.key == SDLK_TAB) {
    FocusNextMenuField((key.mod & SDL_KMOD_SHIFT) != 0 ? -1 : 1);
    return;
  }
  if (key.key == SDLK_RETURN || key.key == SDLK_KP_ENTER) {
    SubmitMenu();
    return;
  }
  if (key.key == SDLK_ESCAPE) {
    menu_.active_field = MenuField::kNone;
    return;
  }
  if (key.key == SDLK_BACKSPACE) {
    if (std::string* buffer = MenuTextBuffer(); buffer != nullptr) {
      EraseLastCodePoint(*buffer);
    }
  }
}

auto App::HandleMenuMouseDown(const SDL_MouseButtonEvent& button) -> void {
  if (button.button != SDL_BUTTON_LEFT || menu_.busy) {
    return;
  }
  int window_width = 0;
  int window_height = 0;
  if (!RenderSize(renderer_, window_width, window_height)) {
    return;
  }
  const MenuLayout layout =
      ComputeMenuLayout(window_width, window_height, menu_.tab);
  if (PointInside(layout.host_tab, button.x, button.y)) {
    menu_.tab = MenuTab::kHost;
    menu_.active_field = MenuField::kNone;
    menu_.status.clear();
    return;
  }
  if (PointInside(layout.join_tab, button.x, button.y)) {
    menu_.tab = MenuTab::kJoin;
    menu_.active_field = MenuField::kNone;
    menu_.status.clear();
    return;
  }
  if (PointInside(layout.editor_tab, button.x, button.y)) {
    menu_.tab = MenuTab::kEditor;
    menu_.active_field = MenuField::kNone;
    menu_.status.clear();
    return;
  }
  if (PointInside(layout.action_button, button.x, button.y)) {
    SubmitMenu();
    return;
  }

  menu_.active_field = MenuField::kNone;
  if (menu_.tab == MenuTab::kHost) {
    if (PointInside(layout.campaign_line, button.x, button.y)) {
      SelectCampaign(1);
    } else if (PointInside(layout.port_field, button.x, button.y)) {
      menu_.active_field = MenuField::kPort;
    } else if (PointInside(layout.password_field, button.x, button.y)) {
      menu_.active_field = MenuField::kPassword;
    }
    return;
  }
  if (menu_.tab == MenuTab::kEditor) {
    return;
  }
  if (PointInside(layout.address_field, button.x, button.y)) {
    menu_.active_field = MenuField::kAddress;
  } else if (PointInside(layout.port_field, button.x, button.y)) {
    menu_.active_field = MenuField::kPort;
  } else if (PointInside(layout.name_field, button.x, button.y)) {
    menu_.active_field = MenuField::kName;
  } else if (PointInside(layout.password_field, button.x, button.y)) {
    menu_.active_field = MenuField::kPassword;
  }
}

auto App::UpdatePlayersFromRoster(const std::vector<RosterEntry>& roster)
    -> void {
  players_.clear();
  player_owners_.clear();
  for (const RosterEntry& entry : roster) {
    players_.push_back(entry.is_dm ? "DM (хост)" : entry.name);
    // У хоста токены принадлежат имени DM, у игроков — своему имени.
    player_owners_.push_back(entry.is_dm ? std::string{} : entry.name);
  }
}

// Клик по списку игроков выбирает первого персонажа этого игрока: это
// быстрый способ открыть его лист, не ища токен на карте.
auto App::SelectPlayerToken(int row) -> void {
  if (row < 0 || static_cast<std::size_t>(row) >= player_owners_.size()) {
    return;
  }
  const std::string& owner = player_owners_[static_cast<std::size_t>(row)];
  if (owner.empty()) {
    return;
  }
  for (const Token& token : view_.Tokens()) {
    if (SamePlayerName(token.owner, owner)) {
      selected_id_ = token.id;
      dragging_ = false;
      drag_target_.reset();
      return;
    }
  }
}

auto App::ApplyUiScale() -> void {
  if (renderer_ == nullptr) {
    return;
  }
  int width = 0;
  int height = 0;
  if (!SDL_GetRenderOutputSize(renderer_, &width, &height) || width <= 0 ||
      height <= 0) {
    return;
  }
  const float scale = kUiScales[static_cast<std::size_t>(ui_scale_index_)];
  int old_width = 0;
  int old_height = 0;
  const bool had_size = RenderSize(renderer_, old_width, old_height);
  // Nearest держит пиксели карты чёткими; увеличенный интерфейс сглаживаем,
  // иначе текст распадается на квадраты.
  SDL_SetDefaultTextureScaleMode(
      renderer_, scale > 1.0F ? SDL_SCALEMODE_LINEAR : SDL_SCALEMODE_NEAREST);
  if (scale <= 1.0F) {
    SDL_SetRenderLogicalPresentation(renderer_, 0, 0,
                                     SDL_LOGICAL_PRESENTATION_DISABLED);
  } else {
    const int logical_width =
        std::max(1, static_cast<int>(static_cast<float>(width) / scale));
    const int logical_height =
        std::max(1, static_cast<int>(static_cast<float>(height) / scale));
    if (!SDL_SetRenderLogicalPresentation(renderer_, logical_width,
                                          logical_height,
                                          SDL_LOGICAL_PRESENTATION_LETTERBOX)) {
      SDL_Log("ui scale failed: %s", SDL_GetError());
    }
  }
  // Камера живёт в координатах интерфейса: после смены масштаба сдвигаем её
  // так, чтобы в центре экрана осталась та же точка карты.
  int new_width = 0;
  int new_height = 0;
  if (had_size && RenderSize(renderer_, new_width, new_height)) {
    PanCamera(
        camera_,
        (static_cast<float>(new_width) - static_cast<float>(old_width)) / 2.0F,
        (static_cast<float>(new_height) - static_cast<float>(old_height)) /
            2.0F);
  }
}

auto App::CycleUiScale(int steps) -> void {
  const int count = static_cast<int>(kUiScales.size());
  ui_scale_index_ = std::clamp(ui_scale_index_ + steps, 0, count - 1);
  ApplyUiScale();
  if (in_game_ && !in_editor_) {
    const auto percent = static_cast<int>(
        kUiScales[static_cast<std::size_t>(ui_scale_index_)] * 100.0F);
    view_.AddLocalLine("Масштаб интерфейса: " + std::to_string(percent) + "%");
  }
}

auto App::LoadFont() -> void {
  if (!TTF_Init()) {
    SDL_Log("TTF_Init failed: %s", SDL_GetError());
    return;
  }
  ttf_initialized_ = true;

  std::vector<std::filesystem::path> candidates;
  if (const char* base = SDL_GetBasePath()) {
    candidates.emplace_back(std::filesystem::path(base) /
                            "assets/fonts/DejaVuSans.ttf");
  }
  candidates.emplace_back("C:/Windows/Fonts/segoeui.ttf");
  candidates.emplace_back("C:/Windows/Fonts/arial.ttf");

  for (const std::filesystem::path& path : candidates) {
    std::error_code error;
    if (!std::filesystem::exists(path, error)) {
      continue;
    }
    font_ = TTF_OpenFont(path.string().c_str(), kFontSize);
    if (font_ != nullptr) {
      TTF_SetFontHinting(font_, TTF_HINTING_NORMAL);
      return;
    }
  }
  SDL_Log("no usable font found; token labels are disabled");
}

auto App::HandleEvent(SDL_Event event) -> void {
  // События мыши приходят в координатах окна: при масштабе интерфейса
  // переводим их в координаты отрисовки, иначе панели не попадут под курсор.
  (void)SDL_ConvertEventToRenderCoordinates(renderer_, &event);
  if (event.type == SDL_EVENT_WINDOW_RESIZED ||
      event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED) {
    ApplyUiScale();
  }
  // Ctrl+= и Ctrl+- меняют масштаб интерфейса в любом режиме и на любой
  // панели; Ctrl+0 возвращает 100%.
  if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat &&
      (event.key.mod & SDL_KMOD_CTRL) != 0) {
    if (event.key.key == SDLK_EQUALS || event.key.key == SDLK_KP_PLUS) {
      CycleUiScale(1);
      return;
    }
    if (event.key.key == SDLK_MINUS || event.key.key == SDLK_KP_MINUS) {
      CycleUiScale(-1);
      return;
    }
    if (event.key.key == SDLK_0) {
      CycleUiScale(-static_cast<int>(kUiScales.size()));
      return;
    }
  }
  // Справку ловим раньше остальных панелей: F1 должен открываться всегда,
  // даже поверх листа персонажа или строки чата.
  if (in_game_ && !in_editor_ && event.type == SDL_EVENT_KEY_DOWN &&
      event.key.key == SDLK_F1 && !event.key.repeat) {
    help_active_ = !help_active_;
    if (help_active_) {
      chat_.Close();
    }
    return;
  }
  if (help_active_ && in_game_ && !in_editor_) {
    if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_ESCAPE) {
      help_active_ = false;
    }
    return;
  }

  if (sheet_active_) {
    switch (event.type) {
      case SDL_EVENT_TEXT_INPUT:
        HandleSheetTextInput(event.text.text);
        break;
      case SDL_EVENT_KEY_DOWN:
        HandleSheetKeyDown(event.key);
        break;
      default:
        break;
    }
    return;
  }

  if (conditions_active_) {
    switch (event.type) {
      case SDL_EVENT_KEY_DOWN:
        HandleConditionsKeyDown(event.key);
        break;
      default:
        break;
    }
    return;
  }

  if (bestiary_active_) {
    switch (event.type) {
      case SDL_EVENT_MOUSE_MOTION:
        mouse_x_ = event.motion.x;
        mouse_y_ = event.motion.y;
        break;
      case SDL_EVENT_KEY_DOWN:
        HandleBestiaryKeyDown(event.key);
        break;
      default:
        break;
    }
    return;
  }

  if (scenes_active_) {
    switch (event.type) {
      case SDL_EVENT_TEXT_INPUT:
        HandleScenesTextInput(event.text.text);
        break;
      case SDL_EVENT_MOUSE_BUTTON_DOWN:
        HandleScenesMouseDown(event.button);
        break;
      case SDL_EVENT_KEY_DOWN:
        HandleScenesKeyDown(event.key);
        break;
      default:
        break;
    }
    return;
  }

  if (in_editor_) {
    if (editor_scenes_active_) {
      switch (event.type) {
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
          HandleEditorScenesMouseDown(event.button);
          break;
        case SDL_EVENT_KEY_DOWN:
          HandleEditorScenesKeyDown(event.key);
          break;
        default:
          break;
      }
      return;
    }
    switch (event.type) {
      case SDL_EVENT_MOUSE_BUTTON_DOWN:
        HandleEditorMouseDown(event.button);
        break;
      case SDL_EVENT_MOUSE_BUTTON_UP:
        HandleEditorMouseUp(event.button);
        break;
      case SDL_EVENT_MOUSE_MOTION:
        HandlePanMotion(event.motion);
        break;
      case SDL_EVENT_MOUSE_WHEEL:
        HandleWheel(event.wheel);
        break;
      case SDL_EVENT_KEY_DOWN:
        HandleEditorKeyDown(event.key);
        break;
      default:
        break;
    }
    return;
  }

  switch (event.type) {
    case SDL_EVENT_TEXT_INPUT:
      if (!in_game_) {
        HandleMenuTextInput(event.text.text);
      } else if (chat_.Active()) {
        chat_.Append(event.text.text);
      }
      break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
      if (!in_game_) {
        HandleMenuMouseDown(event.button);
      } else if (!chat_.Active()) {
        if (event.button.button == SDL_BUTTON_MIDDLE) {
          panning_ = true;
          pan_last_x_ = event.button.x;
          pan_last_y_ = event.button.y;
        } else if (event.button.button == SDL_BUTTON_LEFT) {
          HandleMouseDown(event.button);
        } else if (event.button.button == SDL_BUTTON_RIGHT &&
                   (SDL_GetModState() & SDL_KMOD_ALT) != 0) {
          // Alt+ПКМ — метка «сюда» для всего стола.
          SubmitPing(CellFromPixel(event.button.x, event.button.y));
        }
      }
      break;
    case SDL_EVENT_MOUSE_BUTTON_UP:
      if (in_game_ && !chat_.Active()) {
        if (event.button.button == SDL_BUTTON_MIDDLE) {
          panning_ = false;
        } else if (event.button.button == SDL_BUTTON_LEFT) {
          HandleMouseUp(event.button);
        }
      }
      break;
    case SDL_EVENT_MOUSE_MOTION:
      if (in_game_ && !chat_.Active()) {
        mouse_x_ = event.motion.x;
        mouse_y_ = event.motion.y;
        if (panning_) {
          HandlePanMotion(event.motion);
        } else {
          HandleMouseMotion(event.motion);
        }
      }
      break;
    case SDL_EVENT_MOUSE_WHEEL:
      if (in_game_ && !chat_.Active()) {
        HandleWheel(event.wheel);
      }
      break;
    case SDL_EVENT_KEY_DOWN:
      if (in_game_) {
        HandleKeyDown(event.key);
      } else {
        HandleMenuKeyDown(event.key);
      }
      break;
    default:
      break;
  }
}

auto App::HandleKeyDown(const SDL_KeyboardEvent& key) -> void {
  // Enter в режиме шаблона сразу готовит команду урона по области.
  if ((key.key == SDLK_RETURN || key.key == SDLK_KP_ENTER) && !chat_.Active() &&
      aoe_mode_ && options_.mode == AppMode::kHost) {
    chat_.OpenWith("/aoe ");
    return;
  }
  if (key.key == SDLK_RETURN || key.key == SDLK_KP_ENTER) {
    if (chat_.Active()) {
      SubmitChat();
      chat_.Close();
    } else {
      chat_.Open();
    }
    return;
  }
  if (key.key == SDLK_ESCAPE) {
    if (chat_.Active()) {
      chat_.Close();
    } else {
      dragging_ = false;
      drag_target_.reset();
      measuring_ = false;
      measure_start_.reset();
      measure_end_.reset();
      ClearAoe();
    }
    return;
  }
  if (chat_.Active()) {
    if (key.key == SDLK_BACKSPACE) {
      chat_.Backspace();
    }
    return;
  }
  if ((key.mod & SDL_KMOD_CTRL) != 0 && key.key == SDLK_Z) {
    UndoLastAction();
    return;
  }
  if (HandleAoeKeyDown(key)) {
    return;
  }

  switch (key.key) {
    case SDLK_PAGEUP:
      ScrollLog(kLogVisibleLines);
      break;
    case SDLK_PAGEDOWN:
      ScrollLog(-kLogVisibleLines);
      break;
    case SDLK_END:
      log_scroll_.ToNewest();
      break;
    case SDLK_F5:
      SaveGame();
      break;
    case SDLK_F9:
      LoadGame();
      break;
    case SDLK_C:
      ClaimSelectedToken();
      break;
    case SDLK_V:
      CycleSelectedColor();
      break;
    case SDLK_L:
      OpenSheet();
      break;
    case SDLK_B:
      OpenBestiary();
      break;
    case SDLK_K:
      OpenConditions();
      break;
    case SDLK_M:
      if (FindSelectedToken() == nullptr) {
        view_.AddLocalLine(std::string(strings::kSelectToken));
      } else {
        show_movement_range_ = !show_movement_range_;
      }
      break;
    case SDLK_F:
      if ((key.mod & SDL_KMOD_SHIFT) != 0) {
        ClearAoe();
      } else {
        ToggleAoeMode();
      }
      break;
    case SDLK_R:
      ReleaseSelectedToken();
      break;
    case SDLK_A:
      RollAttack((key.mod & SDL_KMOD_SHIFT) != 0);
      break;
    case SDLK_S:
      RollSave();
      break;
    case SDLK_I:
      RollInitiative();
      break;
    case SDLK_HOME:
      CenterCamera();
      break;
    case SDLK_TAB:
      OpenScenes();
      break;
    default:
      HandleDmHotkey(key.key);
      break;
  }
}

auto App::HandleDmHotkey(SDL_Keycode key) -> void {
  if (options_.mode != AppMode::kHost) {
    return;
  }
  const auto spawn_at_cursor = [this](TokenKind kind) -> void {
    // Создаём строго под текущим курсором: панель лога перекрывает низ
    // карты, поэтому устаревшая клетка наведения поставила бы токен не туда,
    // куда указывал DM.
    const Cell target = CellFromPixel(mouse_x_, mouse_y_);
    if (!view_.Map().Contains(target)) {
      view_.AddLocalLine("Нельзя создать токен за пределами карты");
      return;
    }
    if (view_.Map().IsBlocked(target)) {
      view_.AddLocalLine("Нельзя создать токен в стене");
      return;
    }
    const bool player = kind == TokenKind::kPlayer;
    SendCommand(AddToken{
        .name = player ? "Персонаж" : "Монстр",
        .cell = target,
        .owner = player ? "" : "dm",
        .kind = kind,
        .max_hp = 10,
    });
  };

  switch (key) {
    case SDLK_N:
      spawn_at_cursor(TokenKind::kMonster);
      break;
    case SDLK_P:
      spawn_at_cursor(TokenKind::kPlayer);
      break;
    case SDLK_DELETE:
      RequestDeleteSelectedToken();
      break;
    case SDLK_EQUALS:
    case SDLK_KP_PLUS:
      AdjustSelectedHp(1);
      break;
    case SDLK_MINUS:
    case SDLK_KP_MINUS:
      AdjustSelectedHp(-1);
      break;
    case SDLK_RIGHTBRACKET:
      AdjustSelectedInitiative(1);
      break;
    case SDLK_LEFTBRACKET:
      AdjustSelectedInitiative(-1);
      break;
    case SDLK_T:
      if (view_.EncounterState().active) {
        SendCommand(EndEncounter{});
      } else {
        SendCommand(StartEncounter{});
      }
      break;
    case SDLK_G:
      SendCommand(NextTurn{});
      break;
    default:
      break;
  }
}

auto App::HandleMouseDown(const SDL_MouseButtonEvent& button) -> void {
  // Панели списков рисуются поверх карты и первыми забирают клик.
  const TokenId listed = InitiativeTokenAt(renderer_, text_.get(),
                                           view_.Tokens(), button.x, button.y);
  if (listed != 0) {
    selected_id_ = listed;
    dragging_ = false;
    drag_target_.reset();
    measure_start_.reset();
    measure_end_.reset();
    return;
  }
  const int player_row = PlayerRowAt(renderer_, text_.get(), players_.size(),
                                     players_top_, button.x, button.y);
  if (player_row >= 0) {
    SelectPlayerToken(player_row);
    measure_start_.reset();
    measure_end_.reset();
    return;
  }
  const Cell cell = CellFromPixel(button.x, button.y);
  if (aoe_mode_) {
    mouse_x_ = button.x;
    mouse_y_ = button.y;
    UpdateAoeFromCursor();
    return;
  }
  if ((SDL_GetModState() & SDL_KMOD_ALT) != 0) {
    measuring_ = true;
    measure_start_ = cell;
    measure_end_ = cell;
    return;
  }
  measure_start_.reset();
  measure_end_.reset();
  selected_id_ = TokenAt(cell);
  const Token* token = FindSelectedToken();
  // Повторяем GameServer::CanControl: свободных NPC двигает любой игрок,
  // чужих персонажей и монстров с владельцем тащить нельзя.
  const bool free_npc = token != nullptr && token->owner.empty() &&
                        token->kind != TokenKind::kPlayer;
  const bool locked = token != nullptr && !free_npc &&
                      !SamePlayerName(token->owner, options_.player_name) &&
                      options_.mode == AppMode::kClient;
  dragging_ = selected_id_ != 0 && !locked;
  drag_target_.reset();
  if (dragging_) {
    drag_target_ = cell;
  }
}

auto App::HandleMouseUp(const SDL_MouseButtonEvent& button) -> void {
  if (measuring_) {
    measuring_ = false;
    return;
  }
  if (!dragging_) {
    return;
  }
  const Cell cell = CellFromPixel(button.x, button.y);
  const Token* token = FindSelectedToken();
  // Обычный клик выбирает токен; ход отправляем, только если он сдвинулся.
  if (token != nullptr && view_.Map().Contains(cell) && token->cell != cell) {
    // В неразведанную клетку игрок не ходит: сервер всё равно откажет, а так
    // он получает понятную подсказку без лишней команды.
    if (options_.mode == AppMode::kClient && !client_is_dm_ &&
        !view_.Fog().IsExplored(cell)) {
      view_.AddLocalLine(std::string(strings::kCellNotExplored));
    } else {
      SubmitMove(cell);
    }
  }
  dragging_ = false;
  drag_target_.reset();
}

auto App::HandleMouseMotion(const SDL_MouseMotionEvent& motion) -> void {
  mouse_x_ = motion.x;
  mouse_y_ = motion.y;
  const Cell cell = CellFromPixel(motion.x, motion.y);
  if (aoe_mode_) {
    UpdateAoeFromCursor();
    return;
  }
  if (measuring_) {
    measure_end_ = cell;
    return;
  }
  if (!dragging_) {
    return;
  }
  if (view_.Map().Contains(cell)) {
    drag_target_ = cell;
  }
}

auto App::CellFromPixel(float x, float y) const -> Cell {
  return CameraCellAt(camera_, x, y);
}

auto App::TokenAt(Cell cell) const -> TokenId {
  if (!view_.Fog().IsVisible(cell)) {
    return 0;
  }
  const auto& tokens = view_.Tokens();
  for (const Token& token : tokens | std::views::reverse) {
    if (token.cell == cell) {
      return token.id;
    }
  }
  return 0;
}

auto App::SubmitMove(Cell target) -> void {
  SendCommand(MoveToken{.id = selected_id_, .target = target});
}

auto App::SubmitPing(Cell cell) -> void {
  if (!view_.Map().Contains(cell)) {
    view_.AddLocalLine("Метка за пределами карты");
    return;
  }
  SendCommand(PingAt{.cell = cell});
}

auto App::BackgroundTexture(std::string_view image) -> SDL_Texture* {
  if (image == background_texture_name_) {
    return background_texture_;
  }
  if (background_texture_ != nullptr) {
    SDL_DestroyTexture(background_texture_);
    background_texture_ = nullptr;
  }
  background_texture_name_ = std::string(image);
  if (image.empty() || !IsSafeImageName(image)) {
    return nullptr;
  }
  background_texture_ =
      IMG_LoadTexture(renderer_, AssetPath(image).string().c_str());
  return background_texture_;
}

auto App::PortraitTexture(const std::string& image) -> SDL_Texture* {
  if (image.empty() || !IsSafeImageName(image)) {
    return nullptr;
  }
  const auto found = portrait_textures_.find(image);
  if (found != portrait_textures_.end()) {
    return found->second;
  }
  SDL_Texture* texture =
      IMG_LoadTexture(renderer_, AssetPath(image).string().c_str());
  // Неудачную загрузку тоже запоминаем: не читаем диск каждый кадр.
  portrait_textures_.emplace(image, texture);
  return texture;
}

auto App::DrawBackground(const GridMap& map, const MapBackground& background)
    -> void {
  SDL_Texture* texture = BackgroundTexture(background.image);
  DrawMapBackground(renderer_, texture, map, background.opacity,
                    background.scale, camera_.cell_size, camera_.x, camera_.y);
  if (texture != nullptr || background.image.empty() || text_ == nullptr) {
    return;
  }
  // Файл не найден: сетка остаётся рабочей, а подпись говорит, чего не хватает.
  text_->Draw(std::string(strings::kMapMissingImage) + background.image,
              camera_.x + 8.0F, camera_.y + 8.0F,
              SDL_Color{.r = 0xff, .g = 0x90, .b = 0x90, .a = SDL_ALPHA_OPAQUE},
              640.0F);
}

auto App::SubmitMapCommand(const ChatCommand& command) -> void {
  if (options_.mode != AppMode::kHost) {
    view_.AddLocalLine(std::string(strings::kMapDmOnly));
    return;
  }

  MapBackground next = server_.State().Background();
  switch (command.kind) {
    case ChatCommandKind::kMapOff:
      next = MapBackground{};
      break;
    case ChatCommandKind::kMapOpacity:
      next.opacity = command.value;
      break;
    case ChatCommandKind::kMapScale:
      next.scale = command.value;
      break;
    case ChatCommandKind::kMapImage:
      next.image = command.text;
      break;
    default:
      return;
  }
  SendCommand(SetMapBackground{.background = std::move(next)});
}

auto App::SendCommand(const Command& command) -> void {
  if (options_.mode == AppMode::kClient) {
    net_client_.SendCommand(command);
  } else {
    transport_.SubmitCommand(command);
  }
}

auto App::FindSelectedToken() const -> const Token* {
  if (selected_id_ == 0) {
    return nullptr;
  }
  for (const Token& token : view_.Tokens()) {
    if (token.id == selected_id_) {
      return &token;
    }
  }
  return nullptr;
}

auto App::PlayerHasCharacter() const -> bool {
  return std::ranges::any_of(
      view_.Tokens(), [this](const Token& token) -> bool {
        return token.kind == TokenKind::kPlayer &&
               SamePlayerName(token.owner, options_.player_name);
      });
}

auto App::ClaimSelectedToken() -> void {
  const Token* token = FindSelectedToken();
  if (token == nullptr) {
    view_.AddLocalLine(std::string(strings::kSelectToken));
    return;
  }
  if (options_.mode == AppMode::kHost) {
    view_.AddLocalLine("DM: назначьте владельца командой /owner имя");
    return;
  }
  if (token->kind != TokenKind::kPlayer) {
    view_.AddLocalLine("Забрать можно только персонажа");
    return;
  }
  if (!token->owner.empty() &&
      !SamePlayerName(token->owner, options_.player_name)) {
    view_.AddLocalLine("Этот персонаж уже занят другим игроком");
    return;
  }
  if (!SamePlayerName(token->owner, options_.player_name) &&
      PlayerHasCharacter()) {
    view_.AddLocalLine("У вас уже есть персонаж. Сначала освободите его (R)");
    return;
  }
  SendCommand(ClaimToken{.id = token->id});
}

auto App::ReleaseSelectedToken() -> void {
  const Token* token = FindSelectedToken();
  if (token == nullptr) {
    view_.AddLocalLine(std::string(strings::kSelectToken));
    return;
  }
  if (options_.mode == AppMode::kHost) {
    view_.AddLocalLine("DM: используйте /owner имя");
    return;
  }
  if (!SamePlayerName(token->owner, options_.player_name)) {
    view_.AddLocalLine("Это не ваш персонаж");
    return;
  }
  SendCommand(ReleaseToken{.id = token->id});
}

auto App::CanEditToken(const Token& token) const -> bool {
  if (options_.mode == AppMode::kHost) {
    return true;
  }
  if (token.kind == TokenKind::kPlayer &&
      !SamePlayerName(token.owner, options_.player_name)) {
    return false;
  }
  return token.owner.empty() ||
         SamePlayerName(token.owner, options_.player_name);
}

auto App::OpenSheet() -> void {
  const Token* token = FindSelectedToken();
  if (token == nullptr) {
    view_.AddLocalLine(std::string(strings::kSelectToken));
    return;
  }
  if (!CanEditToken(*token)) {
    view_.AddLocalLine(std::string(strings::kSheetNoAccess));
    return;
  }
  sheet_active_ = true;
  conditions_active_ = false;
  bestiary_active_ = false;
  sheet_token_id_ = token->id;
  sheet_field_ = SheetField::kAc;
  sheet_ac_ = std::to_string(token->ac);
  sheet_speed_ = std::to_string(token->speed);
  sheet_attack_ = std::to_string(token->attack_bonus);
  sheet_save_ = std::to_string(token->save_bonus);
  sheet_initiative_ = std::to_string(token->initiative_bonus);
  sheet_temp_hp_ = std::to_string(token->temp_hp);
  sheet_damage_ = token->damage;
  sheet_notes_ = token->notes;
}

auto App::CloseSheet() -> void {
  sheet_active_ = false;
  sheet_token_id_ = 0;
}

auto App::SaveSheet() -> void {
  int ac = 0;
  int speed = 0;
  int attack = 0;
  int save = 0;
  int initiative = 0;
  int temp_hp = 0;
  if (!ParseNumber(sheet_ac_, ac) || ac > 1000 ||
      !ParseSignedNumber(sheet_speed_, speed) || speed < 0 || speed > 1000 ||
      !ParseSignedNumber(sheet_attack_, attack) || attack < -100 ||
      attack > 100 || !ParseSignedNumber(sheet_save_, save) || save < -100 ||
      save > 100 || !ParseSignedNumber(sheet_initiative_, initiative) ||
      initiative < -100 || initiative > 100 ||
      !ParseSignedNumber(sheet_temp_hp_, temp_hp) || temp_hp < 0 ||
      temp_hp > 1000) {
    view_.AddLocalLine("Некорректные значения полей листа");
    return;
  }
  if (!sheet_damage_.empty()) {
    DiceExpression damage;
    if (!ParseDiceExpression(sheet_damage_, damage)) {
      view_.AddLocalLine("Некорректное выражение урона");
      return;
    }
  }
  const Token* token = nullptr;
  for (const Token& candidate : view_.Tokens()) {
    if (candidate.id == sheet_token_id_) {
      token = &candidate;
      break;
    }
  }
  if (token == nullptr) {
    CloseSheet();
    return;
  }
  SendCommand(UpdateTokenSheet{.id = token->id,
                               .ac = ac,
                               .speed = speed,
                               .notes = sheet_notes_,
                               .attack_bonus = attack,
                               .save_bonus = save,
                               .initiative_bonus = initiative,
                               .damage = sheet_damage_,
                               .temp_hp = temp_hp});
  CloseSheet();
}

auto App::RollAttack(bool advantage) -> void {
  const Token* token = FindSelectedToken();
  if (token == nullptr) {
    view_.AddLocalLine(std::string(strings::kSelectToken));
    return;
  }
  std::string expression = "d20" + FormatBonus(token->attack_bonus);
  if (advantage) {
    expression = "adv " + expression;
  }
  SendCommand(RollExpression{
      .expression = expression,
      .label =
          RollLabel(advantage ? "Атака с преимуществом" : "Атака", token->name),
  });
  if (!token->damage.empty()) {
    SendCommand(RollExpression{.expression = token->damage,
                               .label = RollLabel("Урон", token->name)});
  }
}

auto App::RollSave() -> void {
  const Token* token = FindSelectedToken();
  if (token == nullptr) {
    view_.AddLocalLine(std::string(strings::kSelectToken));
    return;
  }
  SendCommand(RollExpression{
      .expression = "d20" + FormatBonus(token->save_bonus),
      .label = RollLabel("Спасбросок", token->name),
  });
}

auto App::RollInitiative() -> void {
  const Token* token = FindSelectedToken();
  if (token == nullptr) {
    view_.AddLocalLine(std::string(strings::kSelectToken));
    return;
  }
  SendCommand(
      RollExpression{.expression = "d20" + FormatBonus(token->initiative_bonus),
                     .label = RollLabel("Инициатива", token->name)});
}

auto App::HandleSheetTextInput(const char* text) -> void {
  if (text == nullptr) {
    return;
  }
  if (sheet_field_ == SheetField::kAc || sheet_field_ == SheetField::kSpeed ||
      sheet_field_ == SheetField::kAttack ||
      sheet_field_ == SheetField::kSave ||
      sheet_field_ == SheetField::kInitiative) {
    switch (sheet_field_) {
      case SheetField::kAc:
        AppendBoundedNumber(sheet_ac_, text, 1, 1000, false);
        break;
      case SheetField::kSpeed:
        AppendBoundedNumber(sheet_speed_, text, 0, 1000, false);
        break;
      case SheetField::kAttack:
        AppendBoundedNumber(sheet_attack_, text, -100, 100, true);
        break;
      case SheetField::kSave:
        AppendBoundedNumber(sheet_save_, text, -100, 100, true);
        break;
      case SheetField::kInitiative:
        AppendBoundedNumber(sheet_initiative_, text, -100, 100, true);
        break;
      case SheetField::kTempHp:
        AppendBoundedNumber(sheet_temp_hp_, text, 0, 1000, false);
        break;
      default:
        break;
    }
    return;
  }
  if (sheet_field_ == SheetField::kDamage) {
    std::string candidate = sheet_damage_;
    candidate += text;
    if (IsDiceExpressionPrefix(candidate) && candidate.size() <= 64) {
      sheet_damage_ = std::move(candidate);
    }
    return;
  }
  AppendUtf8(sheet_notes_, text, 512);
}

auto App::HandleSheetKeyDown(const SDL_KeyboardEvent& key) -> void {
  if (key.key == SDLK_ESCAPE) {
    CloseSheet();
    return;
  }
  if (key.key == SDLK_RETURN || key.key == SDLK_KP_ENTER) {
    SaveSheet();
    return;
  }
  if (key.key == SDLK_TAB) {
    const int next = (static_cast<int>(sheet_field_) + 1) % 8;
    sheet_field_ = static_cast<SheetField>(next);
    return;
  }
  if (key.key != SDLK_BACKSPACE) {
    return;
  }
  std::string* buffer = nullptr;
  switch (sheet_field_) {
    case SheetField::kAc:
      buffer = &sheet_ac_;
      break;
    case SheetField::kSpeed:
      buffer = &sheet_speed_;
      break;
    case SheetField::kAttack:
      buffer = &sheet_attack_;
      break;
    case SheetField::kSave:
      buffer = &sheet_save_;
      break;
    case SheetField::kInitiative:
      buffer = &sheet_initiative_;
      break;
    case SheetField::kTempHp:
      buffer = &sheet_temp_hp_;
      break;
    case SheetField::kDamage:
      buffer = &sheet_damage_;
      break;
    case SheetField::kNotes:
      buffer = &sheet_notes_;
      break;
  }
  if (buffer != nullptr) {
    EraseLastCodePoint(*buffer);
  }
}

auto App::OpenBestiary() -> void {
  if (options_.mode != AppMode::kHost) {
    view_.AddLocalLine(std::string(strings::kBestiaryDmOnly));
    return;
  }
  bestiary_active_ = true;
  sheet_active_ = false;
  conditions_active_ = false;
}

auto App::HandleBestiaryKeyDown(const SDL_KeyboardEvent& key) -> void {
  if (key.key == SDLK_ESCAPE || key.key == SDLK_B) {
    bestiary_active_ = false;
    return;
  }
  if (key.key >= SDLK_1 && key.key <= SDLK_9) {
    const bool group = (key.mod & SDL_KMOD_SHIFT) != 0;
    SpawnBestiary(static_cast<int>(key.key - SDLK_1), group);
  }
}

auto App::SpawnBestiary(int index, bool group) -> void {
  const auto& bestiary = DefaultBestiary();
  if (index < 0 || std::cmp_greater_equal(index, bestiary.size())) {
    return;
  }
  const Cell origin = CellFromPixel(mouse_x_, mouse_y_);
  if (!view_.Map().Contains(origin) || view_.Map().IsBlocked(origin)) {
    view_.AddLocalLine("Нельзя поставить монстра в эту клетку");
    return;
  }
  const MonsterTemplate& monster = bestiary[static_cast<std::size_t>(index)];
  const int count = group ? MonsterGroupSize(rng_) : 1;
  const std::vector<Cell> cells =
      MonsterGroupCells(view_.Map(), origin, count, view_.Tokens());
  if (cells.empty()) {
    view_.AddLocalLine("Нельзя поставить монстра в эту клетку");
    return;
  }
  const bool numbered = cells.size() > 1;
  for (std::size_t i = 0; i < cells.size(); ++i) {
    std::string name = monster.name;
    if (numbered) {
      name += " " + std::to_string(i + 1);
    }
    SendCommand(AddToken{
        .name = std::move(name),
        .cell = cells[i],
        .owner = "dm",
        .kind = TokenKind::kMonster,
        .max_hp = monster.max_hp,
        .ac = monster.ac,
        .speed = monster.speed,
        .notes = monster.notes,
        .attack_bonus = monster.attack_bonus,
        .save_bonus = monster.save_bonus,
        .damage = monster.damage,
    });
  }
  bestiary_active_ = false;
}

auto App::OpenConditions() -> void {
  const Token* token = FindSelectedToken();
  if (token == nullptr) {
    view_.AddLocalLine(std::string(strings::kSelectToken));
    return;
  }
  if (!CanEditToken(*token)) {
    view_.AddLocalLine(std::string(strings::kSheetNoAccess));
    return;
  }
  conditions_active_ = true;
  conditions_token_id_ = token->id;
  conditions_draft_ = token->conditions;
  sheet_active_ = false;
  bestiary_active_ = false;
}

auto App::CloseConditions() -> void {
  conditions_active_ = false;
  conditions_token_id_ = 0;
  conditions_draft_.clear();
}

auto App::ToggleCondition(int index) -> void {
  const auto& standard = DefaultConditions();
  if (std::cmp_greater_equal(index, standard.size())) {
    return;
  }
  const std::string& condition = standard[static_cast<std::size_t>(index)];
  const auto it = std::ranges::find_if(
      conditions_draft_, [&condition](const Condition& item) -> bool {
        return item.name == condition;
      });
  if (it == conditions_draft_.end()) {
    conditions_draft_.emplace_back(condition, kDefaultConditionRounds);
  } else {
    conditions_draft_.erase(it);
  }
  SendCommand(UpdateTokenCombat{.id = conditions_token_id_,
                                .conditions = conditions_draft_});
}

auto App::HandleConditionsKeyDown(const SDL_KeyboardEvent& key) -> void {
  if (key.key == SDLK_ESCAPE || key.key == SDLK_K) {
    CloseConditions();
    return;
  }
  if (key.key == SDLK_0) {
    conditions_draft_.clear();
    SendCommand(UpdateTokenCombat{.id = conditions_token_id_,
                                  .conditions = conditions_draft_});
    return;
  }
  if (key.key >= SDLK_1 && key.key <= SDLK_8) {
    ToggleCondition(static_cast<int>(key.key - SDLK_1));
  }
}

auto App::SendAoe(const AoeTemplate& aoe) -> void {
  SendCommand(SetAoe{.aoe = aoe});
}

auto App::UpdateAoeFromCursor() -> void {
  if (!aoe_mode_) {
    return;
  }
  const Cell cell = CellFromPixel(mouse_x_, mouse_y_);
  if (!view_.Map().Contains(cell)) {
    return;
  }
  AoeTemplate aoe = view_.Aoe();
  if (aoe.shape == AoeShape::kNone) {
    aoe.shape = AoeShape::kCircle;
  }
  if (aoe.origin == cell) {
    return;
  }
  aoe.origin = cell;
  SendAoe(aoe);
}

auto App::ToggleAoeMode() -> void {
  if (options_.mode != AppMode::kHost) {
    view_.AddLocalLine(std::string(strings::kAoeDmOnly));
    return;
  }
  if (aoe_mode_) {
    aoe_mode_ = false;
    return;
  }
  AoeTemplate aoe = view_.Aoe();
  if (aoe.shape == AoeShape::kNone) {
    aoe = AoeTemplate{.shape = AoeShape::kCircle,
                      .origin = CellFromPixel(mouse_x_, mouse_y_),
                      .direction = 0,
                      .radius = 2,
                      .width = 1};
    if (!view_.Map().Contains(aoe.origin)) {
      aoe.origin = Cell{.x = 0, .y = 0};
    }
    SendAoe(aoe);
  }
  aoe_mode_ = true;
}

auto App::ClearAoe() -> void {
  aoe_mode_ = false;
  if (options_.mode != AppMode::kHost) {
    return;
  }
  AoeTemplate aoe = view_.Aoe();
  if (aoe.shape == AoeShape::kNone) {
    return;
  }
  aoe.shape = AoeShape::kNone;
  SendAoe(aoe);
}

auto App::HandleAoeKeyDown(const SDL_KeyboardEvent& key) -> bool {
  if (!aoe_mode_ || options_.mode != AppMode::kHost) {
    return false;
  }
  const AoeTemplate current = view_.Aoe();
  if (current.shape == AoeShape::kNone) {
    return false;
  }
  auto with_shape = [this, &current](AoeShape shape) -> bool {
    AoeTemplate aoe = current;
    aoe.shape = shape;
    SendAoe(aoe);
    return true;
  };
  switch (key.key) {
    case SDLK_LEFT:
      SendAoe(RotateAoe(current, -1));
      return true;
    case SDLK_RIGHT:
      SendAoe(RotateAoe(current, 1));
      return true;
    case SDLK_UP:
      SendAoe(ResizeAoe(current, 1));
      return true;
    case SDLK_DOWN:
      SendAoe(ResizeAoe(current, -1));
      return true;
    case SDLK_COMMA:
      SendAoe(WidenAoe(current, -1));
      return true;
    case SDLK_PERIOD:
      SendAoe(WidenAoe(current, 1));
      return true;
    case SDLK_1:
      return with_shape(AoeShape::kCircle);
    case SDLK_2:
      return with_shape(AoeShape::kCone);
    case SDLK_3:
      return with_shape(AoeShape::kLine);
    default:
      return false;
  }
}

auto App::CycleSelectedColor() -> void {
  const Token* token = FindSelectedToken();
  if (token == nullptr) {
    view_.AddLocalLine(std::string(strings::kSelectToken));
    return;
  }
  const std::uint32_t current =
      token->color != 0 ? token->color
                        : kTokenPalette[(token->id - 1) % kAutoTokenColorCount];
  std::uint32_t next = kTokenPalette.front();
  for (std::size_t i = 0; i < kTokenPalette.size(); ++i) {
    if (current == kTokenPalette[i]) {
      next = kTokenPalette[(i + 1) % kTokenPalette.size()];
      break;
    }
  }
  SendCommand(SetTokenColor{.id = token->id, .color = next});
}

auto App::SubmitChat() -> void {
  const std::string text = chat_.Take();
  if (text.empty()) {
    return;
  }
  // Синтаксис разбирает ядро; здесь остаются роль, выбранный токен и сцены.
  const ChatCommand command = ParseChatCommand(text);
  if (command.error != ChatCommandError::kNone) {
    view_.AddLocalLine(std::string(ChatUsageText(command.error)));
    return;
  }

  const Token* token = FindSelectedToken();
  switch (command.kind) {
    case ChatCommandKind::kSay:
      SendCommand(Say{.text = command.text});
      return;
    case ChatCommandKind::kRoll:
      SendCommand(RollExpression{.expression = command.text});
      return;
    case ChatCommandKind::kClaim:
      ClaimSelectedToken();
      return;
    case ChatCommandKind::kPing:
      SubmitPing(CellFromPixel(mouse_x_, mouse_y_));
      return;
    case ChatCommandKind::kMapOff:
    case ChatCommandKind::kMapOpacity:
    case ChatCommandKind::kMapScale:
    case ChatCommandKind::kMapImage:
      SubmitMapCommand(command);
      return;
    case ChatCommandKind::kKick:
      if (options_.mode != AppMode::kHost) {
        view_.AddLocalLine(std::string(strings::kKickDmOnly));
        return;
      }
      if (net_server_.KickByName(command.text)) {
        view_.AddLocalLine("Игрок " + command.text + " исключён");
      } else {
        view_.AddLocalLine("Игрок " + command.text + " не найден");
      }
      return;
    case ChatCommandKind::kSceneList:
    case ChatCommandKind::kSceneNew:
    case ChatCommandKind::kSceneRename:
    case ChatCommandKind::kSceneSwitch:
      if (options_.mode != AppMode::kHost) {
        view_.AddLocalLine(std::string(strings::kScenesDmOnly));
        return;
      }
      if (command.kind == ChatCommandKind::kSceneList) {
        OpenScenes();
      } else if (command.kind == ChatCommandKind::kSceneNew) {
        CreateScene();
      } else if (command.kind == ChatCommandKind::kSceneRename) {
        RenameSceneByCommand(command.text);
      } else {
        SwitchSceneByName(command.text);
      }
      return;
    case ChatCommandKind::kAoe:
      if (options_.mode != AppMode::kHost) {
        view_.AddLocalLine("Урон по области наносит только DM");
        return;
      }
      SendCommand(
          AoeDamage{.expression = command.text, .save_dc = command.save_dc});
      return;
    case ChatCommandKind::kOwner:
      if (token == nullptr) {
        view_.AddLocalLine(std::string(strings::kSelectTokenForCommand));
        return;
      }
      SendCommand(SetTokenOwner{.id = token->id, .owner = command.text});
      return;
    case ChatCommandKind::kRename:
      if (token == nullptr) {
        view_.AddLocalLine(std::string(strings::kSelectTokenForCommand));
        return;
      }
      SendCommand(RenameToken{.id = token->id, .name = command.text});
      return;
    case ChatCommandKind::kPortrait:
      if (token == nullptr) {
        view_.AddLocalLine(std::string(strings::kSelectTokenForCommand));
        return;
      }
      SendCommand(SetTokenPortrait{.id = token->id, .portrait = command.text});
      return;
    case ChatCommandKind::kMaxHp:
      if (token == nullptr) {
        view_.AddLocalLine(std::string(strings::kSelectTokenForCommand));
        return;
      }
      SendCommand(SetTokenMaxHp{.id = token->id, .max_hp = command.value});
      return;
    case ChatCommandKind::kDamage:
    case ChatCommandKind::kHeal:
      if (token == nullptr) {
        view_.AddLocalLine(std::string(strings::kSelectTokenForCommand));
        return;
      }
      if (command.kind == ChatCommandKind::kHeal) {
        SendCommand(HealToken{.id = token->id, .amount = command.value});
      } else {
        SendCommand(DamageToken{.id = token->id, .amount = command.value});
      }
      return;
    case ChatCommandKind::kDeathSave:
      if (token == nullptr) {
        view_.AddLocalLine(std::string(strings::kSelectTokenForCommand));
        return;
      }
      if (token->hp > 0) {
        view_.AddLocalLine("Спасбросок от смерти нужен при 0 HP");
        return;
      }
      SendCommand(DeathSave{.id = token->id});
      return;
    case ChatCommandKind::kRest:
      if (options_.mode != AppMode::kHost) {
        view_.AddLocalLine("Отдых объявляет только DM");
        return;
      }
      SendCommand(RestParty{.long_rest = command.long_rest});
      return;
    case ChatCommandKind::kReveal:
    case ChatCommandKind::kRevealOff: {
      if (options_.mode != AppMode::kHost) {
        view_.AddLocalLine(std::string(strings::kRevealDmOnly));
        return;
      }
      if (command.kind == ChatCommandKind::kRevealOff) {
        SendCommand(ClearRevealed{});
        return;
      }
      const Cell reveal_cell = CellFromPixel(mouse_x_, mouse_y_);
      if (!view_.Map().Contains(reveal_cell)) {
        view_.AddLocalLine("Открывать можно только клетки карты");
        return;
      }
      SendCommand(
          RevealArea{.cell = reveal_cell, .radius = kRevealRadiusCells});
      return;
    }
    case ChatCommandKind::kXp:
      if (options_.mode != AppMode::kHost) {
        view_.AddLocalLine(std::string(strings::kXpDmOnly));
        return;
      }
      if (command.party) {
        SendCommand(AwardXp{.amount = command.value, .party = true});
        return;
      }
      if (token == nullptr) {
        view_.AddLocalLine(std::string(strings::kSelectTokenForCommand));
        return;
      }
      SendCommand(AwardXp{.id = token->id, .amount = command.value});
      return;
  }
}

auto App::AdjustSelectedHp(int delta) -> void {
  if (selected_id_ == 0) {
    return;
  }
  for (const Token& token : view_.Tokens()) {
    if (token.id != selected_id_) {
      continue;
    }
    const int hp = std::clamp(token.hp + delta, 0, token.max_hp);
    SendCommand(SetTokenHp{.id = token.id, .hp = hp});
    return;
  }
}

auto App::AdjustSelectedInitiative(int delta) -> void {
  if (selected_id_ == 0) {
    return;
  }
  for (const Token& token : view_.Tokens()) {
    if (token.id != selected_id_) {
      continue;
    }
    SendCommand(
        SetInitiative{.id = token.id, .value = token.initiative + delta});
    return;
  }
}

auto App::ApplyOutcome(PeerId from, const CommandOutcome& outcome) -> void {
  if (!outcome.accepted) {
    if (from != 0) {
      net_server_.Reject(from, outcome.reject_reason);
      SDL_Log("rejected command from peer %u: %s", static_cast<unsigned>(from),
              outcome.reject_reason.c_str());
    }
    view_.AddLocalLine(std::string(strings::kRejected) + outcome.reject_reason);
    return;
  }
  // Чат и броски приходят как LogLineAdded: мир они не меняют, автосейв не
  // нужен. Всё остальное — движение, бой, листы — меняет сцену.
  const bool world_changed =
      std::ranges::any_of(outcome.events, [](const Event& event) -> bool {
        return !std::holds_alternative<LogLineAdded>(event);
      });
  if (world_changed) {
    MarkCampaignDirty();
  }
  const std::vector<PeerInfo> peers = net_server_.Peers();
  for (const Event& event : outcome.events) {
    // Вид DM уже полный: ему нужны только перемещения токенов и строки лога.
    // Игрокам уходит только то, что попадает в их собственный туман.
    if (!std::holds_alternative<ViewUpdated>(event)) {
      transport_.PostEvent(event);
    }
    for (const PeerInfo& peer : peers) {
      if (server_.EventVisibleTo(event, peer.name)) {
        net_server_.SendEvent(peer.id, event);
      }
    }
  }
  // После изменения мира каждому игроку нужен свой срез: туман у всех разный,
  // поэтому один общий ViewUpdated больше не подходит.
  if (world_changed) {
    for (const PeerInfo& peer : peers) {
      net_server_.SendEvent(
          peer.id, ViewUpdated{.snapshot = server_.ClientSnapshot(peer.name),
                               .fog = server_.FogState(peer.name)});
    }
  }
}

auto App::PumpHost() -> void {
  while (const std::optional<Command> command = transport_.NextCommand()) {
    const GameSnapshot before = server_.Snapshot();
    const CommandOutcome outcome = server_.HandleCommand(*command, "", true);
    // В историю отмены попадают только действия, меняющие мир, а не чат.
    const bool world_changed =
        std::ranges::any_of(outcome.events, [](const Event& event) -> bool {
          return !std::holds_alternative<LogLineAdded>(event);
        });
    if (outcome.accepted && world_changed) {
      PushUndo(before);
    }
    ApplyOutcome(0, outcome);
  }

  std::vector<PeerJoined> joined;
  std::vector<PeerCommand> commands;
  std::vector<PeerId> dropped;
  net_server_.Poll(joined, commands, dropped);

  for (const PeerId peer : dropped) {
    SDL_Log("peer %u disconnected", static_cast<unsigned>(peer));
  }
  for (const PeerJoined& peer : joined) {
    SDL_Log("peer %u '%s' joined", static_cast<unsigned>(peer.peer),
            peer.name.c_str());
    net_server_.SendWelcome(
        peer.peer, Welcome{.snapshot = server_.ClientSnapshot(peer.name),
                           .fog = server_.FogState(peer.name),
                           .is_dm = false});
  }
  for (const PeerCommand& peer_command : commands) {
    ApplyOutcome(peer_command.peer,
                 server_.HandleCommand(peer_command.command,
                                       peer_command.player_name, false));
  }

  while (const std::optional<Event> event = transport_.NextEvent()) {
    view_.Apply(*event);
  }

  players_.clear();
  players_.emplace_back("DM (хост)");
  player_owners_.clear();
  player_owners_.emplace_back();
  for (const PeerInfo& peer : net_server_.Peers()) {
    players_.push_back(peer.name);
    player_owners_.push_back(peer.name);
  }
}

auto App::PumpClient() -> void {
  const auto now = std::chrono::steady_clock::now();
  if (!in_game_ && client_connecting_ && now >= connect_deadline_) {
    menu_.status = "Не удалось подключиться к хосту";
    menu_.busy = false;
    client_connecting_ = false;
    net_client_.Disconnect();
    return;
  }

  NetClientUpdate update;
  net_client_.Poll(update);

  if (update.welcome.has_value()) {
    const bool reconnected = in_game_ && !client_connected_;
    view_.Reset(update.welcome->snapshot, update.welcome->fog);
    PruneStaleViewState();
    client_connected_ = true;
    client_is_dm_ = update.welcome->is_dm;
    client_connecting_ = false;
    in_game_ = true;
    menu_.busy = false;
    menu_.status.clear();
    CenterCamera();
    if (reconnected) {
      view_.AddLocalLine(std::string(strings::kConnectionRestored));
    }
    SDL_Log("joined host, world snapshot received");
  }
  for (const Event& event : update.events) {
    view_.Apply(event);
  }
  if (update.roster.has_value()) {
    UpdatePlayersFromRoster(*update.roster);
  }
  if (update.reject.has_value()) {
    if (in_game_ && net_client_.Welcomed()) {
      view_.AddLocalLine(std::string(strings::kRejected) + *update.reject);
    } else if (in_game_) {
      menu_.status = std::string(strings::kRejected) + *update.reject;
      menu_.busy = false;
      client_connecting_ = false;
      in_game_ = false;
      net_client_.Disconnect();
    } else {
      menu_.status = std::string(strings::kRejected) + *update.reject;
      menu_.busy = false;
      client_connecting_ = false;
      net_client_.Disconnect();
    }
    SDL_Log("host rejected request: %s", update.reject->c_str());
  }
  if (update.disconnected) {
    if (in_game_) {
      if (client_connected_) {
        SDL_Log("connection to host lost, reconnecting...");
        view_.AddLocalLine(std::string(strings::kConnectionLost));
      }
    } else {
      menu_.status = "Не удалось подключиться к хосту";
      menu_.busy = false;
      client_connecting_ = false;
    }
    client_connected_ = false;
  }

  if (in_game_ && !net_client_.Connected() && now >= next_reconnect_) {
    next_reconnect_ = now + kReconnectDelay;
    // Попытки подключения неблокирующие; успех определяется по приходу
    // Welcome, поэтому повторы не шумят.
    net_client_.Reconnect();
  }
}

auto App::PruneStaleViewState() -> void {
  const auto has_token = [this](TokenId id) -> bool {
    return id != 0 && std::ranges::any_of(view_.Tokens(),
                                          [id](const Token& token) -> bool {
                                            return token.id == id;
                                          });
  };
  if (!has_token(selected_id_)) {
    selected_id_ = 0;
    dragging_ = false;
    drag_target_.reset();
    measuring_ = false;
    measure_start_.reset();
    measure_end_.reset();
  }
  if (sheet_active_ && !has_token(sheet_token_id_)) {
    CloseSheet();
  }
  if (conditions_active_ && !has_token(conditions_token_id_)) {
    CloseConditions();
  }
}

auto App::ScrollLog(int lines) -> void {
  log_scroll_.Scroll(lines, view_.Log().size());
}

auto App::SyncLogScroll() -> void { log_scroll_.OnAppend(view_.Log().size()); }

auto App::OpenSessionLog() -> void {
  const std::string path = SessionLogPath().string();
  if (!session_log_.Open(path, CurrentTimeSeconds())) {
    SDL_Log("session log is unavailable: %s", path.c_str());
  }
}

auto App::FlushSessionLog() -> void {
  const std::uint64_t appended = view_.AppendedLogLines();
  if (!session_log_.IsOpen()) {
    log_written_ = static_cast<std::size_t>(appended);
    return;
  }
  // Буфер ограничен, поэтому пишем не больше, чем в нём лежит.
  const std::size_t pending = std::min<std::size_t>(
      static_cast<std::size_t>(appended - log_written_), view_.Log().size());
  const std::size_t first = view_.Log().size() - pending;
  for (std::size_t i = first; i < view_.Log().size(); ++i) {
    session_log_.Append(view_.Log()[i]);
  }
  log_written_ = static_cast<std::size_t>(appended);
  session_log_.Flush();
}

auto App::SaveGame() -> void {
  if (options_.mode == AppMode::kClient) {
    SDL_Log("saving is only available on the host");
    return;
  }
  if (WriteSnapshot(server_.Snapshot(), SavePath())) {
    // Кампания хранит тот же мир, что и сохранение хоста.
    StoreActiveScene();
    SaveCampaign();
    SDL_Log("game saved");
  } else {
    SDL_Log("save failed");
  }
}

auto App::LoadGame() -> void {
  if (options_.mode == AppMode::kClient) {
    SDL_Log("loading is only available on the host");
    return;
  }
  const std::optional<GameSnapshot> snapshot = ReadSnapshot(SavePath());
  if (!snapshot.has_value()) {
    SDL_Log("load failed");
    return;
  }
  ResetWorld(*snapshot);
  // Загруженный мир становится активной сценой кампании.
  StoreActiveScene();
  SaveCampaign();
  SDL_Log("game loaded");
}

auto App::SaveCampaign() -> void {
  const std::string file = campaign_files_.empty()
                               ? std::string("campaign.dndcamp")
                               : campaign_files_[campaign_index_];
  if (!WriteCampaign(campaign_, AssetPath(file))) {
    SDL_Log("campaign save failed");
    return;
  }
  autosave_.Saved();
}

auto App::RefreshCampaignLibrary() -> void {
  const std::string current = campaign_files_.empty()
                                  ? std::string{}
                                  : campaign_files_[campaign_index_];
  campaign_files_ = ListCampaignFiles(BaseDirectory());
  if (campaign_files_.empty()) {
    campaign_files_.emplace_back("campaign.dndcamp");
    campaign_index_ = 0;
    return;
  }
  campaign_index_ = 0;
  const std::string target =
      current.empty() ? std::string("campaign.dndcamp") : current;
  for (std::size_t i = 0; i < campaign_files_.size(); ++i) {
    if (campaign_files_[i] == target) {
      campaign_index_ = i;
      return;
    }
  }
}

auto App::LoadSelectedCampaign() -> bool {
  if (campaign_files_.empty()) {
    return false;
  }
  const std::string& file = campaign_files_[campaign_index_];
  const std::filesystem::path path = AssetPath(file);
  std::error_code error;
  if (!std::filesystem::exists(path, error)) {
    // Пустой слот: кампания создастся при первом сохранении.
    return false;
  }
  const std::optional<Campaign> loaded = ReadCampaign(path);
  if (!loaded.has_value()) {
    menu_.status = "Не удалось прочитать " + file;
    return false;
  }
  campaign_ = *loaded;
  scene_ = campaign_.scenes[campaign_.active].snapshot;
  scene_.encounter = Encounter{};
  server_.Reset(scene_);
  view_.Reset(server_.DmSnapshot(), server_.FullFogState());
  menu_.status.clear();
  return true;
}

auto App::SelectCampaign(int delta) -> void {
  if (campaign_files_.empty() || delta == 0) {
    return;
  }
  campaign_index_ = WrapIndex(campaign_index_, delta, campaign_files_.size());
  (void)LoadSelectedCampaign();
}

auto App::MarkCampaignDirty() -> void {
  autosave_.MarkChanged(std::chrono::steady_clock::now());
}

auto App::CheckAutosave() -> void {
  if (!autosave_.Dirty() || options_.mode != AppMode::kHost || !in_game_) {
    return;
  }
  const auto now = std::chrono::steady_clock::now();
  if (!autosave_.Due(now)) {
    return;
  }
  StoreActiveScene();
  SaveCampaign();
  if (autosave_.Dirty()) {
    // Запись не удалась: не долбим диск каждый кадр, повторим позже.
    autosave_.SaveFailed(now);
  } else {
    SDL_Log("campaign autosaved");
  }
}

auto App::StoreActiveScene() -> void {
  if (campaign_.scenes.empty() || campaign_.active >= campaign_.scenes.size()) {
    return;
  }
  campaign_.scenes[campaign_.active].snapshot = server_.Snapshot();
}

auto App::BroadcastWorld() -> void {
  for (const PeerInfo& peer : net_server_.Peers()) {
    net_server_.SendEvent(
        peer.id, ViewUpdated{.snapshot = server_.ClientSnapshot(peer.name),
                             .fog = server_.FogState(peer.name)});
  }
}

auto App::Announce(std::string text) -> void {
  const LogLineAdded event{
      .line =
          LogLine{
              .author = "DM",
              .text = std::move(text),
              .timestamp = CurrentTimeSeconds(),
          },
  };
  view_.Apply(event);
  net_server_.BroadcastEvent(event);
}

auto App::ResetWorld(GameSnapshot snapshot) -> void {
  ClearUndo();
  // Бой принадлежит прошлому миру: в новой сцене он начинается заново.
  ApplyWorld(std::move(snapshot), true);
}

auto App::RestoreUndoWorld(GameSnapshot snapshot) -> void {
  // Отмена возвращает и бой: состояние до действия должно совпасть.
  ApplyWorld(std::move(snapshot), false);
}

auto App::ApplyWorld(GameSnapshot snapshot, bool reset_encounter) -> void {
  if (reset_encounter) {
    snapshot.encounter = Encounter{};
  }
  server_.Reset(std::move(snapshot));
  scene_ = server_.Snapshot();
  transport_ = LocalTransport{};
  selected_id_ = 0;
  dragging_ = false;
  drag_target_.reset();
  measuring_ = false;
  measure_start_.reset();
  measure_end_.reset();
  show_movement_range_ = false;
  aoe_mode_ = false;
  chat_.Close();
  sheet_active_ = false;
  conditions_active_ = false;
  bestiary_active_ = false;
  delete_token_.Clear();
  delete_scene_.Clear();
  view_.Reset(server_.DmSnapshot(), server_.FullFogState());
  CenterCamera();
  BroadcastWorld();
}

auto App::PushUndo(GameSnapshot snapshot) -> void {
  undo_stack_.push_back(std::move(snapshot));
  while (undo_stack_.size() > kUndoDepth) {
    undo_stack_.pop_front();
  }
}

auto App::ClearUndo() -> void { undo_stack_.clear(); }

auto App::UndoLastAction() -> void {
  if (options_.mode != AppMode::kHost || !in_game_) {
    view_.AddLocalLine(std::string(strings::kUndoDmOnly));
    return;
  }
  if (undo_stack_.empty()) {
    view_.AddLocalLine(std::string(strings::kUndoEmpty));
    return;
  }
  GameSnapshot snapshot = std::move(undo_stack_.back());
  undo_stack_.pop_back();
  RestoreUndoWorld(std::move(snapshot));
  MarkCampaignDirty();
  view_.AddLocalLine(std::string(strings::kUndoDone));
}

auto App::RequestDeleteSelectedToken() -> void {
  if (selected_id_ == 0) {
    view_.AddLocalLine(std::string(strings::kSelectToken));
    return;
  }
  if (delete_token_.Request(static_cast<std::size_t>(selected_id_),
                            std::chrono::steady_clock::now())) {
    SendCommand(RemoveToken{.id = selected_id_});
    return;
  }
  view_.AddLocalLine(std::string(strings::kDeleteConfirm));
}

auto App::RequestDeleteActiveScene() -> void {
  if (options_.mode != AppMode::kHost) {
    view_.AddLocalLine(std::string(strings::kScenesDmOnly));
    return;
  }
  if (campaign_.scenes.size() <= 1) {
    scenes_status_ = "Нельзя удалить последнюю сцену";
    return;
  }
  if (delete_scene_.Request(campaign_.active,
                            std::chrono::steady_clock::now())) {
    scenes_status_.clear();
    DeleteActiveScene();
    return;
  }
  scenes_status_ = std::string(strings::kDeleteConfirm);
}

auto App::SwitchScene(std::size_t index) -> void {
  if (options_.mode != AppMode::kHost) {
    view_.AddLocalLine(std::string(strings::kScenesDmOnly));
    return;
  }
  if (index >= campaign_.scenes.size()) {
    view_.AddLocalLine(std::string(strings::kScenesEmpty));
    return;
  }
  StoreActiveScene();
  if (index == campaign_.active) {
    SaveCampaign();
    return;
  }
  campaign_.active = index;
  const std::string name = campaign_.scenes[index].name;
  ResetWorld(campaign_.scenes[index].snapshot);
  SDL_Log("scene '%s' is active", name.c_str());
  Announce("Сцена: " + name);
  SaveCampaign();
}

auto App::SwitchSceneByName(std::string_view name) -> void {
  for (std::size_t index = 0; index < campaign_.scenes.size(); ++index) {
    if (campaign_.scenes[index].name == name) {
      SwitchScene(index);
      return;
    }
  }
  view_.AddLocalLine(std::string(strings::kScenesEmpty));
}

auto App::CreateScene() -> void {
  if (options_.mode != AppMode::kHost) {
    view_.AddLocalLine(std::string(strings::kScenesDmOnly));
    return;
  }
  if (campaign_.scenes.size() >= kMaxCampaignScenes) {
    view_.AddLocalLine("Достигнут предел числа сцен");
    return;
  }
  StoreActiveScene();
  ClearUndo();
  // Новая сцена — копия текущей: DM сразу расставляет в ней монстров.
  std::string name = NextSceneName(campaign_);
  if (!dnd::IsSceneNameAvailable(campaign_, name)) {
    view_.AddLocalLine(std::string(strings::kSceneNameTaken));
    return;
  }
  Scene scene{.name = std::move(name),
              .snapshot = campaign_.scenes[campaign_.active].snapshot};
  const std::string display_name = scene.name;
  campaign_.scenes.push_back(std::move(scene));
  campaign_.active = campaign_.scenes.size() - 1;
  scenes_selected_ = campaign_.active;
  SaveCampaign();
  Announce("Новая сцена: " + display_name);
}

auto App::DeleteActiveScene() -> void {
  if (options_.mode != AppMode::kHost) {
    view_.AddLocalLine(std::string(strings::kScenesDmOnly));
    return;
  }
  if (campaign_.scenes.size() <= 1) {
    view_.AddLocalLine("Нельзя удалить последнюю сцену");
    return;
  }
  const std::size_t removed = campaign_.active;
  const std::string name = campaign_.scenes[removed].name;
  campaign_.scenes.erase(campaign_.scenes.begin() +
                         static_cast<std::ptrdiff_t>(removed));
  const std::size_t next = IndexAfterDelete(removed, campaign_.scenes.size());
  campaign_.active = next;
  scenes_selected_ = next;
  ResetWorld(campaign_.scenes[next].snapshot);
  SDL_Log("scene '%s' deleted", name.c_str());
  Announce("Сцена удалена: " + name);
  SaveCampaign();
}

auto App::OpenScenes() -> void {
  if (options_.mode != AppMode::kHost) {
    view_.AddLocalLine(std::string(strings::kScenesDmOnly));
    return;
  }
  scenes_active_ = true;
  scenes_selected_ = campaign_.active;
  scenes_edit_ = SceneEditField::kNone;
  scenes_edit_buffer_.clear();
  scenes_status_.clear();
  bestiary_active_ = false;
  sheet_active_ = false;
  conditions_active_ = false;
  chat_.Close();
}

auto App::CloseScenes() -> void {
  scenes_active_ = false;
  CancelSceneEdit();
  scenes_status_.clear();
}

auto App::HandleScenesMouseDown(const SDL_MouseButtonEvent& button) -> void {
  if (button.button != SDL_BUTTON_LEFT ||
      scenes_edit_ != SceneEditField::kNone) {
    return;
  }
  const std::size_t index = SceneRowAt(renderer_, text_.get(), campaign_.scenes,
                                       scenes_selected_, button.x, button.y);
  if (index == kNoScene) {
    return;
  }
  scenes_selected_ = index;
  scenes_status_.clear();
  if (button.clicks >= 2) {
    CloseScenes();
    SwitchScene(index);
  }
}

auto App::HandleScenesKeyDown(const SDL_KeyboardEvent& key) -> void {
  if (scenes_edit_ != SceneEditField::kNone) {
    if (key.key == SDLK_ESCAPE) {
      CancelSceneEdit();
    } else if (key.key == SDLK_RETURN || key.key == SDLK_KP_ENTER) {
      CommitSceneEdit();
    } else if (key.key == SDLK_BACKSPACE) {
      EraseLastCodePoint(scenes_edit_buffer_);
      scenes_status_.clear();
    }
    return;
  }
  switch (key.key) {
    case SDLK_ESCAPE:
    case SDLK_TAB:
      CloseScenes();
      return;
    case SDLK_UP:
      scenes_selected_ =
          StepIndex(scenes_selected_, -1, campaign_.scenes.size());
      return;
    case SDLK_DOWN:
      scenes_selected_ =
          StepIndex(scenes_selected_, 1, campaign_.scenes.size());
      return;
    case SDLK_RETURN:
    case SDLK_KP_ENTER: {
      const std::size_t index = scenes_selected_;
      CloseScenes();
      SwitchScene(index);
      return;
    }
    case SDLK_R:
      BeginSceneEdit(SceneEditField::kName);
      return;
    case SDLK_E:
      BeginSceneEdit(SceneEditField::kNote);
      return;
    case SDLK_N:
      CreateScene();
      return;
    case SDLK_DELETE:
      RequestDeleteActiveScene();
      return;
    default:
      break;
  }
  if (key.key >= SDLK_1 && key.key <= SDLK_9) {
    const std::optional<std::size_t> index = IndexForDigit(
        static_cast<int>(key.key - SDLK_1) + 1, campaign_.scenes.size());
    if (index.has_value()) {
      scenes_selected_ = *index;
      CloseScenes();
      SwitchScene(*index);
    }
  }
}

auto App::HandleScenesTextInput(const char* text) -> void {
  if (scenes_edit_ == SceneEditField::kNone || text == nullptr) {
    return;
  }
  if (scenes_edit_ignore_text_) {
    scenes_edit_ignore_text_ = false;
    return;
  }
  AppendUtf8(scenes_edit_buffer_, text,
             scenes_edit_ == SceneEditField::kName ? kMaxSceneNameLength
                                                   : kMaxSceneNoteLength);
  scenes_status_.clear();
}

auto App::RenameSceneAt(std::size_t index, std::string name) -> bool {
  if (options_.mode != AppMode::kHost) {
    view_.AddLocalLine(std::string(strings::kScenesDmOnly));
    return false;
  }
  if (index >= campaign_.scenes.size()) {
    view_.AddLocalLine(std::string(strings::kScenesEmpty));
    return false;
  }
  if (name.empty()) {
    view_.AddLocalLine(std::string(strings::kSceneNameEmpty));
    return false;
  }
  TruncateUtf8(name, kMaxSceneNameLength);
  const std::string old_name = campaign_.scenes[index].name;
  if (!dnd::RenameScene(campaign_, index, std::move(name))) {
    view_.AddLocalLine(std::string(strings::kSceneNameTaken));
    return false;
  }
  scenes_selected_ = index;
  SaveCampaign();
  Announce("Сцена переименована: " + old_name + " → " +
           campaign_.scenes[index].name);
  return true;
}

auto App::RenameSceneByCommand(std::string_view args) -> void {
  const std::optional<SceneRenameRequest> request =
      ParseSceneRename(campaign_, args);
  if (!request.has_value()) {
    view_.AddLocalLine(TrimText(args).empty()
                           ? std::string(strings::kSceneRenameUsage)
                           : std::string(strings::kScenesEmpty));
    return;
  }
  (void)RenameSceneAt(request->index, request->new_name);
}

auto App::BeginSceneEdit(SceneEditField field) -> void {
  if (field == SceneEditField::kNone ||
      scenes_selected_ >= campaign_.scenes.size()) {
    return;
  }
  const Scene& scene = campaign_.scenes[scenes_selected_];
  scenes_edit_ = field;
  // Имя набирается с нуля: так переименование не требует стирать старое.
  scenes_edit_buffer_ =
      field == SceneEditField::kName ? std::string{} : scene.note;
  scenes_edit_ignore_text_ = true;
  scenes_status_.clear();
}

auto App::CancelSceneEdit() -> void {
  scenes_edit_ = SceneEditField::kNone;
  scenes_edit_buffer_.clear();
  scenes_edit_ignore_text_ = false;
}

auto App::CommitSceneEdit() -> void {
  if (scenes_selected_ >= campaign_.scenes.size()) {
    CancelSceneEdit();
    return;
  }
  if (scenes_edit_ == SceneEditField::kName) {
    std::string name(TrimText(scenes_edit_buffer_));
    if (name.empty()) {
      scenes_status_ = std::string(strings::kSceneNameEmpty);
      return;
    }
    if (!RenameSceneAt(scenes_selected_, std::move(name))) {
      scenes_status_ = std::string(strings::kSceneNameTaken);
      return;
    }
  } else if (scenes_edit_ == SceneEditField::kNote) {
    std::string note(TrimText(scenes_edit_buffer_));
    TruncateUtf8(note, kMaxSceneNoteLength);
    campaign_.scenes[scenes_selected_].note = std::move(note);
    SaveCampaign();
  }
  CancelSceneEdit();
  scenes_status_.clear();
}

auto App::Iterate() -> void {
  if (text_ != nullptr) {
    text_->BeginFrame();
  }

  if (in_editor_) {
    ClearBoard(renderer_);
    DrawBackground(scene_.map, scene_.background);
    DrawGrid(renderer_, scene_.map, camera_.cell_size, camera_.x, camera_.y);
    DrawWalls(renderer_, scene_.map, camera_.cell_size, camera_.x, camera_.y);
    std::vector<TokenArt> editor_art;
    editor_art.reserve(scene_.tokens.size());
    for (const Token& token : scene_.tokens) {
      editor_art.push_back(
          TokenArt{.portrait = PortraitTexture(token.portrait)});
    }
    DrawTokens(renderer_, scene_.tokens, camera_.cell_size, camera_.x,
               camera_.y, text_.get(), 0, 0, &editor_art);
    if (editor_panel_visible_) {
      DrawEditorPanel(renderer_, text_.get(), scene_, editor_brush_,
                      editor_status_);
    }
    if (editor_scenes_active_) {
      DrawScenesPanel(renderer_, text_.get(), campaign_.scenes,
                      campaign_.active, editor_scenes_selected_, "", "",
                      editor_scenes_status_, strings::kEditorScenesHint);
    }
    SDL_RenderPresent(renderer_);
    return;
  }

  if (options_.mode == AppMode::kClient && (client_connecting_ || in_game_)) {
    PumpClient();
  } else if (in_game_) {
    PumpHost();
  }
  SyncLogScroll();
  FlushSessionLog();
  CheckAutosave();

  if (!in_game_) {
    menu_.scene_count = campaign_.scenes.size();
    menu_.scene_name = campaign_.active < campaign_.scenes.size()
                           ? campaign_.scenes[campaign_.active].name
                           : std::string{};
    menu_.campaign_count = campaign_files_.size();
    menu_.campaign_index = campaign_index_;
    menu_.campaign_file = campaign_files_.empty()
                              ? std::string{}
                              : campaign_files_[campaign_index_];
    DrawMenu(renderer_, text_.get(), menu_, local_addresses_);
    SDL_RenderPresent(renderer_);
    return;
  }

  // При перетаскивании выбранный токен рисуется на клетке под курсором;
  // сервер узнаёт о ходе только при отпускании кнопки. Токены вне текущей
  // зоны видимости не показываются вообще.
  std::vector<Token> shown;
  std::vector<TokenArt> art;
  for (const Token& token : view_.Tokens()) {
    if (!view_.Fog().IsVisible(token.cell)) {
      continue;
    }
    shown.push_back(token);
    art.push_back(TokenArt{.portrait = PortraitTexture(token.portrait)});
    if (dragging_ && token.id == selected_id_ && drag_target_.has_value()) {
      shown.back().cell = *drag_target_;
    }
  }

  const bool is_dm = options_.mode == AppMode::kHost || client_is_dm_;
  ClearBoard(renderer_);
  DrawBackground(view_.Map(), view_.Background());
  DrawGrid(renderer_, view_.Map(), camera_.cell_size, camera_.x, camera_.y);
  DrawWalls(renderer_, view_.Map(), camera_.cell_size, camera_.x, camera_.y);
  const Encounter& encounter = view_.EncounterState();
  if (show_movement_range_) {
    const Token* range_token = FindSelectedToken();
    if (range_token != nullptr) {
      int radius = SpeedToCells(range_token->speed);
      if (encounter.active && !is_dm) {
        radius = std::max(0, radius - range_token->movement_used);
      }
      // Монстры и NPC перекрывают путь, персонажи — нет; встать на занятую
      // клетку всё равно нельзя, поэтому её не показываем.
      std::vector<Cell> blockers;
      std::vector<Cell> occupied;
      for (const Token& token : view_.Tokens()) {
        if (token.id == range_token->id) {
          continue;
        }
        occupied.push_back(token.cell);
        if (token.kind != TokenKind::kPlayer) {
          blockers.push_back(token.cell);
        }
      }
      std::vector<Cell> reachable =
          ReachableCells(view_.Map(), range_token->cell, radius,
                         range_token->diagonal_parity, blockers);
      std::erase_if(reachable, [&occupied](Cell cell) -> bool {
        return std::ranges::find(occupied, cell) != occupied.end();
      });
      DrawMovementRange(renderer_, reachable, camera_.cell_size, camera_.x,
                        camera_.y);
    }
  }
  if (!client_is_dm_) {
    DrawFog(renderer_, view_.Fog(), camera_.cell_size, camera_.x, camera_.y);
  }
  // Шаблон области рисуем поверх тумана: он показывает заклинание, а не рельеф.
  if (view_.Aoe().shape != AoeShape::kNone) {
    DrawAoe(renderer_, AoeCells(view_.Map(), view_.Aoe()), camera_.cell_size,
            camera_.x, camera_.y);
  }
  const TokenId active_id =
      encounter.active && encounter.turn_index < encounter.order.size()
          ? encounter.order[encounter.turn_index]
          : 0;
  DrawTokens(renderer_, shown, camera_.cell_size, camera_.x, camera_.y,
             text_.get(), selected_id_, active_id, &art);
  const std::uint32_t now = CurrentTimeSeconds();
  view_.PrunePings(now);
  DrawPings(renderer_, view_.Pings(), now, camera_.cell_size, camera_.x,
            camera_.y, text_.get());
  if (measure_start_.has_value() && measure_end_.has_value()) {
    DrawRuler(renderer_, *measure_start_, *measure_end_, camera_.cell_size,
              camera_.x, camera_.y);
    const int dx = std::abs(measure_end_->x - measure_start_->x);
    const int dy = std::abs(measure_end_->y - measure_start_->y);
    const int cells = std::max(dx, dy);
    const float label_x =
        camera_.x +
        ((static_cast<float>(measure_start_->x + measure_end_->x) + 1.0F) *
         camera_.cell_size / 2.0F);
    const float label_y =
        camera_.y +
        ((static_cast<float>(measure_start_->y + measure_end_->y) + 1.0F) *
         camera_.cell_size / 2.0F);
    text_->Draw(
        FormatMeters(cells), label_x + 8.0F, label_y - 8.0F,
        SDL_Color{.r = 0xf5, .g = 0xe0, .b = 0x8c, .a = SDL_ALPHA_OPAQUE},
        120.0F);
  }
  DrawInitiativePanel(renderer_, text_.get(), view_.Tokens(), encounter);
  if (aoe_mode_) {
    text_->Draw(
        std::string(strings::kAoeHint), 16.0F, 16.0F,
        SDL_Color{.r = 0xf5, .g = 0xe0, .b = 0x8c, .a = SDL_ALPHA_OPAQUE},
        960.0F);
  }
  const bool is_client = options_.mode == AppMode::kClient;
  const Token* selected = FindSelectedToken();
  const bool can_claim = is_client && selected != nullptr &&
                         selected->kind == TokenKind::kPlayer &&
                         selected->owner.empty() && !PlayerHasCharacter();
  const bool can_release =
      is_client && selected != nullptr &&
      SamePlayerName(selected->owner, options_.player_name);
  MovementInfo movement = MovementInfo::kFree;
  if (is_dm) {
    movement = MovementInfo::kHidden;
  } else if (encounter.active) {
    movement = MovementInfo::kBudget;
  }
  const float token_bottom =
      DrawTokenPanel(renderer_, text_.get(), selected, options_.player_name,
                     is_client, can_claim, can_release, movement);
  const float players_y = token_bottom > 0.0F ? token_bottom + 12.0F : 280.0F;
  players_top_ = players_y;
  DrawPlayersPanel(renderer_, text_.get(), players_, players_y);
  if (dragging_ && drag_target_.has_value()) {
    DrawCellHighlight(renderer_, *drag_target_, camera_.cell_size, camera_.x,
                      camera_.y);
  }
  log_top_ =
      DrawLogPanel(renderer_, text_.get(), view_.Log(), chat_.Active(),
                   std::string(chat_.Text()), options_.mode == AppMode::kHost,
                   log_scroll_.Offset());
  if (sheet_active_) {
    const Token* sheet_token = nullptr;
    for (const Token& token : view_.Tokens()) {
      if (token.id == sheet_token_id_) {
        sheet_token = &token;
        break;
      }
    }
    if (sheet_token == nullptr) {
      CloseSheet();
    } else {
      DrawSheetPanel(renderer_, text_.get(), *sheet_token, sheet_field_,
                     sheet_ac_, sheet_speed_, sheet_attack_, sheet_save_,
                     sheet_initiative_, sheet_temp_hp_, sheet_damage_,
                     sheet_notes_);
    }
  }
  if (bestiary_active_) {
    DrawBestiaryPanel(renderer_, text_.get(), DefaultBestiary());
  }
  if (scenes_active_) {
    std::string edit_label;
    if (scenes_edit_ == SceneEditField::kName) {
      edit_label = std::string(strings::kSceneNameLabel);
    } else if (scenes_edit_ == SceneEditField::kNote) {
      edit_label = std::string(strings::kSceneNoteLabel);
    }
    DrawScenesPanel(renderer_, text_.get(), campaign_.scenes, campaign_.active,
                    scenes_selected_, edit_label, scenes_edit_buffer_,
                    scenes_status_, strings::kScenesHint);
  }
  if (conditions_active_) {
    const Token* condition_token = nullptr;
    for (const Token& token : view_.Tokens()) {
      if (token.id == conditions_token_id_) {
        condition_token = &token;
        break;
      }
    }
    if (condition_token == nullptr) {
      CloseConditions();
    } else {
      DrawConditionsPanel(renderer_, text_.get(), *condition_token,
                          conditions_draft_);
    }
  }
  // Клиент остаётся на последнем снапшоте, пока идёт переподключение; баннер
  // нужен, чтобы он не принял замороженный стол за настоящий.
  if (is_client && !client_connected_) {
    DrawConnectionBanner(renderer_, text_.get(), strings::kConnectionLost);
  }
  // Справка рисуется последней и перекрывает все остальные панели.
  if (help_active_) {
    DrawHelpPanel(renderer_, text_.get(), is_dm);
  }
  SDL_RenderPresent(renderer_);
}

}  // namespace dnd
