#include "app/app.h"

#include <SDL3_ttf/SDL_ttf.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
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
#include "core/conditions.h"
#include "core/dice.h"
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
constexpr std::chrono::seconds kReconnectDelay{2};
constexpr std::size_t kMaxChatInput = 200;

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

auto AppendBoundedNumber(std::string& buffer, const char* text, int minimum,
                         int maximum, bool allow_negative) -> void {
  if (text == nullptr) {
    return;
  }
  for (const char* it = text; *it != '\0'; ++it) {
    if ((*it < '0' || *it > '9') &&
        !(allow_negative && *it == '-' && buffer.empty())) {
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

auto FormatBonus(int bonus) -> std::string {
  if (bonus > 0) {
    return "+" + std::to_string(bonus);
  }
  if (bonus < 0) {
    return std::to_string(bonus);
  }
  return {};
}

auto ParsePortText(std::string_view text, std::uint16_t& port) -> bool {
  int value = 0;
  if (!ParseNumber(text, value) || value > 65535) {
    return false;
  }
  port = static_cast<std::uint16_t>(value);
  return true;
}

auto PointInside(const SDL_FRect& rect, float x, float y) -> bool {
  return x >= rect.x && y >= rect.y && x < rect.x + rect.w &&
         y < rect.y + rect.h;
}

auto MenuTextFieldLimit(MenuField field) -> std::size_t {
  switch (field) {
    case MenuField::kAddress:
      return 128;
    case MenuField::kPort:
      return 5;
    case MenuField::kName:
      return 64;
    case MenuField::kPassword:
      return 128;
    case MenuField::kNone:
      break;
  }
  return 0;
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
  // Nearest-сэмплинг сохраняет текст чётким при масштабировании из-за DPI.
  SDL_SetDefaultTextureScaleMode(renderer_, SDL_SCALEMODE_NEAREST);
  // Итерации по vsync; безопасно, если драйвер не поддерживает vsync.
  SDL_SetRenderVSync(renderer_, 1);

  LoadFont();
  text_ = std::make_unique<TextRenderer>(renderer_, font_);
  if (!text_->Valid()) {
    SDL_Log("text renderer initialization failed: %s", SDL_GetError());
  }
  local_addresses_ = LocalIpv4Addresses();

  const std::optional<GameSnapshot> loaded = ReadSnapshot(ScenePath());
  if (loaded.has_value()) {
    scene_ = *loaded;
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
  if (!ParsePortText(menu_.port, port)) {
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
  GameSnapshot start = scene_;
  start.encounter = Encounter{};
  server_.Reset(std::move(start));
  in_game_ = true;
  in_editor_ = false;
  client_connecting_ = false;
  client_connected_ = false;
  menu_.busy = false;
  menu_.status.clear();
  selected_id_ = 0;
  dragging_ = false;
  drag_target_.reset();
  chat_active_ = false;
  chat_input_.clear();
  players_.clear();
  players_.push_back("DM (хост)");
  local_addresses_ = LocalIpv4Addresses();
  view_.Reset(server_.DmSnapshot(), server_.FullFogState());
  CenterCamera();
  SDL_Log("hosting on port %u", static_cast<unsigned>(port));
  return true;
}

auto App::StartClient() -> bool {
  std::uint16_t port = 0;
  if (!ParsePortText(menu_.port, port)) {
    menu_.status = "Некорректный порт";
    return false;
  }
  if (menu_.name.empty()) {
    menu_.status = "Введите имя игрока";
    return false;
  }

  std::string host = menu_.address;
  const auto colon = host.rfind(':');
  if (colon != std::string::npos && colon + 1 < host.size()) {
    std::uint16_t embedded_port = 0;
    if (ParsePortText(std::string_view(host).substr(colon + 1),
                      embedded_port)) {
      port = embedded_port;
      host.resize(colon);
    }
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
  selected_id_ = 0;
  dragging_ = false;
  drag_target_.reset();
  panning_ = false;
  chat_active_ = false;
  chat_input_.clear();
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
  editor_status_ = WriteSnapshot(scene_, ScenePath())
                       ? std::string(strings::kEditorSceneSaved)
                       : "Не удалось сохранить сцену";
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
  if (!SDL_GetRenderOutputSize(renderer_, &window_width, &window_height)) {
    return;
  }
  const GridMap& map = in_editor_ ? scene_.map : view_.Map();
  camera_x_ = (static_cast<float>(window_width) -
               (static_cast<float>(map.Width()) * cell_size_)) /
              2.0F;
  camera_y_ = (static_cast<float>(window_height) -
               (static_cast<float>(map.Height()) * cell_size_)) /
              2.0F;
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
      return;
    case SDLK_TAB:
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
  camera_x_ += static_cast<float>(motion.x) - pan_last_x_;
  camera_y_ += static_cast<float>(motion.y) - pan_last_y_;
  pan_last_x_ = static_cast<float>(motion.x);
  pan_last_y_ = static_cast<float>(motion.y);
}

auto App::HandleWheel(const SDL_MouseWheelEvent& wheel) -> void {
  if (wheel.y == 0.0F) {
    return;
  }
  const float old_size = cell_size_;
  const float world_x = (wheel.mouse_x - camera_x_) / old_size;
  const float world_y = (wheel.mouse_y - camera_y_) / old_size;
  cell_size_ = std::clamp(cell_size_ + (wheel.y * 4.0F), 24.0F, 96.0F);
  camera_x_ = wheel.mouse_x - (world_x * cell_size_);
  camera_y_ = wheel.mouse_y - (world_y * cell_size_);
}

auto App::MenuTextBuffer() -> std::string* {
  if (menu_.tab == MenuTab::kEditor) {
    return nullptr;
  }
  switch (menu_.active_field) {
    case MenuField::kAddress:
      return menu_.tab == MenuTab::kJoin ? &menu_.address : nullptr;
    case MenuField::kPort:
      return &menu_.port;
    case MenuField::kName:
      return menu_.tab == MenuTab::kJoin ? &menu_.name : nullptr;
    case MenuField::kPassword:
      return &menu_.password;
    case MenuField::kNone:
      break;
  }
  return nullptr;
}

auto App::FocusNextMenuField(int direction) -> void {
  if (menu_.tab == MenuTab::kEditor) {
    menu_.active_field = MenuField::kNone;
    return;
  }
  std::array<MenuField, 4> fields{};
  int count = 0;
  if (menu_.tab == MenuTab::kHost) {
    fields[count++] = MenuField::kPort;
    fields[count++] = MenuField::kPassword;
  } else {
    fields[count++] = MenuField::kAddress;
    fields[count++] = MenuField::kPort;
    fields[count++] = MenuField::kName;
    fields[count++] = MenuField::kPassword;
  }

  int current = -1;
  for (int i = 0; i < count; ++i) {
    if (fields[static_cast<std::size_t>(i)] == menu_.active_field) {
      current = i;
      break;
    }
  }
  int next = 0;
  if (current < 0) {
    next = direction >= 0 ? 0 : count - 1;
  } else {
    next = (current + direction + count) % count;
  }
  menu_.active_field = fields[static_cast<std::size_t>(next)];
}

auto App::HandleMenuTextInput(const char* text) -> void {
  std::string* buffer = MenuTextBuffer();
  if (buffer == nullptr || text == nullptr) {
    return;
  }
  *buffer += text;
  TruncateUtf8(*buffer, MenuTextFieldLimit(menu_.active_field));
}

auto App::HandleMenuKeyDown(const SDL_KeyboardEvent& key) -> void {
  if (key.key == SDLK_LEFT || key.key == SDLK_RIGHT) {
    const int step = key.key == SDLK_RIGHT ? 1 : 2;
    menu_.tab = static_cast<MenuTab>((static_cast<int>(menu_.tab) + step) % 3);
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
    std::string* buffer = MenuTextBuffer();
    if (buffer != nullptr && !buffer->empty()) {
      buffer->resize(PreviousCodePointStart(*buffer));
    }
  }
}

auto App::HandleMenuMouseDown(const SDL_MouseButtonEvent& button) -> void {
  if (button.button != SDL_BUTTON_LEFT || menu_.busy) {
    return;
  }
  int window_width = 0;
  int window_height = 0;
  if (!SDL_GetRenderOutputSize(renderer_, &window_width, &window_height)) {
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
    if (PointInside(layout.port_field, button.x, button.y)) {
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
  for (const RosterEntry& entry : roster) {
    players_.push_back(entry.is_dm ? "DM (хост)" : entry.name);
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

auto App::HandleEvent(const SDL_Event& event) -> void {
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

  if (in_editor_) {
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
      } else if (chat_active_) {
        chat_input_ += event.text.text;
        TruncateUtf8(chat_input_, kMaxChatInput);
      }
      break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
      if (!in_game_) {
        HandleMenuMouseDown(event.button);
      } else if (!chat_active_) {
        if (event.button.button == SDL_BUTTON_MIDDLE) {
          panning_ = true;
          pan_last_x_ = event.button.x;
          pan_last_y_ = event.button.y;
        } else if (event.button.button == SDL_BUTTON_LEFT) {
          HandleMouseDown(event.button);
        }
      }
      break;
    case SDL_EVENT_MOUSE_BUTTON_UP:
      if (in_game_ && !chat_active_) {
        if (event.button.button == SDL_BUTTON_MIDDLE) {
          panning_ = false;
        } else if (event.button.button == SDL_BUTTON_LEFT) {
          HandleMouseUp(event.button);
        }
      }
      break;
    case SDL_EVENT_MOUSE_MOTION:
      if (in_game_ && !chat_active_) {
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
      if (in_game_ && !chat_active_) {
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
  if (key.key == SDLK_RETURN || key.key == SDLK_KP_ENTER) {
    if (chat_active_) {
      SubmitChat();
      chat_active_ = false;
    } else {
      chat_active_ = true;
      chat_input_.clear();
    }
    return;
  }
  if (key.key == SDLK_ESCAPE) {
    if (chat_active_) {
      chat_active_ = false;
      chat_input_.clear();
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
  if (chat_active_) {
    if (key.key == SDLK_BACKSPACE && !chat_input_.empty()) {
      chat_input_.resize(PreviousCodePointStart(chat_input_));
    }
    return;
  }
  if (HandleAoeKeyDown(key)) {
    return;
  }

  switch (key.key) {
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
      if (selected_id_ != 0) {
        SendCommand(RemoveToken{.id = selected_id_});
      }
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
  const bool locked = token != nullptr && token->kind == TokenKind::kPlayer &&
                      token->owner != options_.player_name &&
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
    SubmitMove(cell);
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
  return Cell{
      .x = static_cast<int>(std::floor((x - camera_x_) / cell_size_)),
      .y = static_cast<int>(std::floor((y - camera_y_) / cell_size_)),
  };
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
  for (const Token& token : view_.Tokens()) {
    if (token.kind == TokenKind::kPlayer &&
        token.owner == options_.player_name) {
      return true;
    }
  }
  return false;
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
  if (!token->owner.empty() && token->owner != options_.player_name) {
    view_.AddLocalLine("Этот персонаж уже занят другим игроком");
    return;
  }
  if (token->owner != options_.player_name && PlayerHasCharacter()) {
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
  if (token->owner != options_.player_name) {
    view_.AddLocalLine("Это не ваш персонаж");
    return;
  }
  SendCommand(ReleaseToken{.id = token->id});
}

auto App::CanEditToken(const Token& token) const -> bool {
  if (options_.mode == AppMode::kHost) {
    return true;
  }
  if (token.kind == TokenKind::kPlayer && token.owner != options_.player_name) {
    return false;
  }
  return token.owner.empty() || token.owner == options_.player_name;
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
  int temp_hp = 0;
  if (!ParseNumber(sheet_ac_, ac) || ac > 1000 ||
      !ParseSignedNumber(sheet_speed_, speed) || speed < 0 || speed > 1000 ||
      !ParseSignedNumber(sheet_attack_, attack) || attack < -100 ||
      attack > 100 || !ParseSignedNumber(sheet_save_, save) || save < -100 ||
      save > 100 || !ParseSignedNumber(sheet_temp_hp_, temp_hp) ||
      temp_hp < 0 || temp_hp > 1000) {
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
      .label = advantage ? "Атака с преимуществом" : "Атака",
  });
  if (!token->damage.empty()) {
    SendCommand(RollExpression{.expression = token->damage, .label = "Урон"});
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
      .label = "Спасбросок",
  });
}

auto App::RollInitiative() -> void {
  if (FindSelectedToken() == nullptr) {
    view_.AddLocalLine(std::string(strings::kSelectToken));
    return;
  }
  SendCommand(RollExpression{.expression = "d20", .label = "Инициатива"});
}

auto App::HandleSheetTextInput(const char* text) -> void {
  if (text == nullptr) {
    return;
  }
  if (sheet_field_ == SheetField::kAc || sheet_field_ == SheetField::kSpeed ||
      sheet_field_ == SheetField::kAttack ||
      sheet_field_ == SheetField::kSave) {
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
  sheet_notes_ += text;
  TruncateUtf8(sheet_notes_, 512);
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
    const int next = (static_cast<int>(sheet_field_) + 1) % 7;
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
  if (buffer != nullptr && !buffer->empty()) {
    buffer->resize(PreviousCodePointStart(*buffer));
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
  if (key.key >= SDLK_1 && key.key <= SDLK_5) {
    SpawnBestiary(static_cast<int>(key.key - SDLK_1));
  }
}

auto App::SpawnBestiary(int index) -> void {
  const auto& bestiary = DefaultBestiary();
  if (index < 0 || index >= static_cast<int>(bestiary.size())) {
    return;
  }
  const Cell cell = CellFromPixel(mouse_x_, mouse_y_);
  if (!view_.Map().Contains(cell) || view_.Map().IsBlocked(cell)) {
    view_.AddLocalLine("Нельзя поставить монстра в эту клетку");
    return;
  }
  const MonsterTemplate& monster = bestiary[static_cast<std::size_t>(index)];
  SendCommand(AddToken{
      .name = monster.name,
      .cell = cell,
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
  if (index < 0 || index >= static_cast<int>(standard.size())) {
    return;
  }
  const std::string& condition = standard[static_cast<std::size_t>(index)];
  const auto it = std::ranges::find(conditions_draft_, condition);
  if (it == conditions_draft_.end()) {
    conditions_draft_.push_back(condition);
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
  const std::string text = chat_input_;
  chat_input_.clear();
  if (text.empty()) {
    return;
  }
  if (text.starts_with("/roll ")) {
    const std::string expression = text.substr(6);
    DiceExpression parsed;
    if (!ParseDiceExpression(expression, parsed)) {
      view_.AddLocalLine("Использование: /roll d20+5, 2d6+3, adv d20+5");
      return;
    }
    SendCommand(RollExpression{.expression = expression});
    return;
  }
  if (text == "/claim") {
    ClaimSelectedToken();
    return;
  }
  if (options_.mode == AppMode::kHost && text.starts_with("/kick ")) {
    const std::string name = text.substr(6);
    if (name.empty()) {
      view_.AddLocalLine("Использование: /kick имя");
    } else if (net_server_.KickByName(name)) {
      view_.AddLocalLine("Игрок " + name + " исключён");
    } else {
      view_.AddLocalLine("Игрок " + name + " не найден");
    }
    return;
  }

  const Token* token = FindSelectedToken();
  if (text == "/owner" || text.starts_with("/owner ")) {
    if (token == nullptr) {
      view_.AddLocalLine(std::string(strings::kSelectTokenForCommand));
      return;
    }
    const std::string owner = text.size() > 7 ? text.substr(7) : std::string{};
    SendCommand(SetTokenOwner{.id = token->id, .owner = owner});
    return;
  }
  if (text.starts_with("/rename ")) {
    if (token == nullptr) {
      view_.AddLocalLine(std::string(strings::kSelectTokenForCommand));
      return;
    }
    const std::string name = text.substr(8);
    if (name.empty()) {
      view_.AddLocalLine("Использование: /rename имя");
      return;
    }
    SendCommand(RenameToken{.id = token->id, .name = name});
    return;
  }
  if (text.starts_with("/maxhp ")) {
    if (token == nullptr) {
      view_.AddLocalLine(std::string(strings::kSelectTokenForCommand));
      return;
    }
    int max_hp = 0;
    if (!ParseNumber(std::string_view(text).substr(7), max_hp)) {
      view_.AddLocalLine("Использование: /maxhp 20");
      return;
    }
    SendCommand(SetTokenMaxHp{.id = token->id, .max_hp = max_hp});
    return;
  }
  SendCommand(Say{.text = text});
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
  for (const Event& event : outcome.events) {
    // Вид DM уже полный: ему нужны только перемещения токенов и строки лога.
    // Игроки дополнительно получают обновления вида и туман.
    if (!std::holds_alternative<ViewUpdated>(event)) {
      transport_.PostEvent(event);
    }
    if (server_.EventVisibleToPlayers(event)) {
      net_server_.BroadcastEvent(event);
    }
  }
}

auto App::PumpHost() -> void {
  while (const std::optional<Command> command = transport_.NextCommand()) {
    ApplyOutcome(0, server_.HandleCommand(*command, "", true));
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
    net_server_.SendWelcome(peer.peer,
                            Welcome{.snapshot = server_.ClientSnapshot(),
                                    .fog = server_.FogState(),
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
  players_.push_back("DM (хост)");
  for (const PeerInfo& peer : net_server_.Peers()) {
    players_.push_back(peer.name);
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
    view_.Reset(update.welcome->snapshot, update.welcome->fog);
    client_connected_ = true;
    client_is_dm_ = update.welcome->is_dm;
    client_connecting_ = false;
    in_game_ = true;
    menu_.busy = false;
    menu_.status.clear();
    CenterCamera();
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

auto App::SaveGame() -> void {
  if (options_.mode == AppMode::kClient) {
    SDL_Log("saving is only available on the host");
    return;
  }
  if (WriteSnapshot(server_.Snapshot(), SavePath())) {
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
  server_.Reset(*snapshot);
  view_.Reset(server_.DmSnapshot(), server_.FullFogState());
  transport_ = LocalTransport{};
  selected_id_ = 0;
  dragging_ = false;
  aoe_mode_ = false;
  drag_target_.reset();
  CenterCamera();
  SDL_Log("game loaded");
}

auto App::Iterate() -> void {
  if (text_ != nullptr) {
    text_->BeginFrame();
  }

  if (in_editor_) {
    DrawGrid(renderer_, scene_.map, cell_size_, camera_x_, camera_y_);
    DrawWalls(renderer_, scene_.map, cell_size_, camera_x_, camera_y_);
    DrawTokens(renderer_, scene_.tokens, cell_size_, camera_x_, camera_y_,
               text_.get(), 0, 0);
    if (editor_panel_visible_) {
      DrawEditorPanel(renderer_, text_.get(), scene_, editor_brush_,
                      editor_status_);
    }
    SDL_RenderPresent(renderer_);
    return;
  }

  if (options_.mode == AppMode::kClient && (client_connecting_ || in_game_)) {
    PumpClient();
  } else if (in_game_) {
    PumpHost();
  }

  if (!in_game_) {
    DrawMenu(renderer_, text_.get(), menu_, local_addresses_);
    SDL_RenderPresent(renderer_);
    return;
  }

  // При перетаскивании выбранный токен рисуется на клетке под курсором;
  // сервер узнаёт о ходе только при отпускании кнопки. Токены вне текущей
  // зоны видимости не показываются вообще.
  std::vector<Token> shown;
  for (const Token& token : view_.Tokens()) {
    if (!view_.Fog().IsVisible(token.cell)) {
      continue;
    }
    shown.push_back(token);
    if (dragging_ && token.id == selected_id_ && drag_target_.has_value()) {
      shown.back().cell = *drag_target_;
    }
  }

  const bool is_dm = options_.mode == AppMode::kHost || client_is_dm_;
  DrawGrid(renderer_, view_.Map(), cell_size_, camera_x_, camera_y_);
  DrawWalls(renderer_, view_.Map(), cell_size_, camera_x_, camera_y_);
  const Encounter& encounter = view_.EncounterState();
  if (show_movement_range_) {
    const Token* range_token = FindSelectedToken();
    if (range_token != nullptr) {
      int radius = SpeedToCells(range_token->speed);
      if (encounter.active && !is_dm) {
        radius = std::max(0, radius - range_token->movement_used);
      }
      DrawMovementRange(renderer_,
                        ReachableCells(view_.Map(), range_token->cell, radius),
                        cell_size_, camera_x_, camera_y_);
    }
  }
  if (!client_is_dm_) {
    DrawFog(renderer_, view_.Fog(), cell_size_, camera_x_, camera_y_);
  }
  // Шаблон области рисуем поверх тумана: он показывает заклинание, а не рельеф.
  if (view_.Aoe().shape != AoeShape::kNone) {
    DrawAoe(renderer_, AoeCells(view_.Map(), view_.Aoe()), cell_size_,
            camera_x_, camera_y_);
  }
  const TokenId active_id =
      encounter.active && encounter.turn_index < encounter.order.size()
          ? encounter.order[encounter.turn_index]
          : 0;
  DrawTokens(renderer_, shown, cell_size_, camera_x_, camera_y_, text_.get(),
             selected_id_, active_id);
  if (measure_start_.has_value() && measure_end_.has_value()) {
    DrawRuler(renderer_, *measure_start_, *measure_end_, cell_size_, camera_x_,
              camera_y_);
    const int dx = std::abs(measure_end_->x - measure_start_->x);
    const int dy = std::abs(measure_end_->y - measure_start_->y);
    const int cells = std::max(dx, dy);
    const float label_x =
        camera_x_ +
        ((static_cast<float>(measure_start_->x + measure_end_->x) + 1.0F) *
         cell_size_ / 2.0F);
    const float label_y =
        camera_y_ +
        ((static_cast<float>(measure_start_->y + measure_end_->y) + 1.0F) *
         cell_size_ / 2.0F);
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
  const bool can_release = is_client && selected != nullptr &&
                           selected->owner == options_.player_name;
  const float token_bottom = DrawTokenPanel(
      renderer_, text_.get(), selected, options_.player_name, is_client,
      can_claim, can_release,
      is_dm ? MovementInfo::kHidden
            : (encounter.active ? MovementInfo::kBudget : MovementInfo::kFree));
  const float players_y = token_bottom > 0.0F ? token_bottom + 12.0F : 280.0F;
  DrawPlayersPanel(renderer_, text_.get(), players_, players_y);
  if (dragging_ && drag_target_.has_value()) {
    DrawCellHighlight(renderer_, *drag_target_, cell_size_, camera_x_,
                      camera_y_);
  }
  DrawLogPanel(renderer_, text_.get(), view_.Log(), chat_active_, chat_input_,
               options_.mode == AppMode::kHost);
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
                     sheet_temp_hp_, sheet_damage_, sheet_notes_);
    }
  }
  if (bestiary_active_) {
    DrawBestiaryPanel(renderer_, text_.get(), DefaultBestiary());
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
  SDL_RenderPresent(renderer_);
}

}  // namespace dnd
