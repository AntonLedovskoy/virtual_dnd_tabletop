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

#include "app/icon_png.h"
#include "app/strings.h"
#include "app/update_service.h"
#include "core/bestiary.h"
#include "core/clock.h"
#include "core/conditions.h"
#include "core/damage.h"
#include "core/dice.h"
#include "core/editor_token.h"
#include "core/levels.h"
#include "core/movement.h"
#include "core/navigation.h"
#include "core/player_name.h"
#include "core/scene_io.h"
#include "core/ui_hint.h"
#include "core/units.h"
#include "core/update.h"
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

// Следующий цвет палитры после текущего; токен без своего цвета начинает
// с автоматического цвета по id.
auto NextTokenColor(const Token& token) -> std::uint32_t {
  const std::uint32_t current =
      token.color != 0 ? token.color
                       : kTokenPalette[(token.id - 1) % kAutoTokenColorCount];
  for (std::size_t i = 0; i < kTokenPalette.size(); ++i) {
    if (current == kTokenPalette[i]) {
      return kTokenPalette[(i + 1) % kTokenPalette.size()];
    }
  }
  return kTokenPalette.front();
}

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

// Шаблон кампании не несёт следов партии: туман, бой, часы и открытые
// области остаются в оверлее. `revealed` тоже след партии: в редакторе его
// не расставляют, а шаблон должен начинаться с чистого обзора.
auto CleanTemplateSnapshot(GameSnapshot snapshot) -> GameSnapshot {
  snapshot.fog.clear();
  snapshot.revealed.clear();
  snapshot.encounter = Encounter{};
  snapshot.game_time_seconds = 0;
  snapshot.last_long_rest_seconds = -1;
  return snapshot;
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
    case ChatCommandError::kBadTime:
      return strings::kTimeUsage;
    case ChatCommandError::kBadRest:
      return strings::kRestUsage;
    case ChatCommandError::kBadFall:
      return strings::kFallUsage;
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

// «0.3.0 (протокол 42)»: в диалоге версий нужны оба числа.
auto VersionWithProtocol(const Version& version, std::uint32_t protocol)
    -> std::string {
  return FormatVersion(version) + " (" + std::string(strings::kProtocolLabel) +
         std::to_string(protocol) + ")";
}

// Иконка окна лежит в exe (src/app/icon_png.h): игра не ищет файл рядом с
// собой, а окно и панель задач выглядят одинаково.
auto LoadWindowIcon() -> SDL_Surface* {
  SDL_IOStream* stream = SDL_IOFromConstMem(kIconPng.data(), kIconPng.size());
  if (stream == nullptr) {
    return nullptr;
  }
  SDL_Surface* surface = IMG_LoadPNG_IO(stream);
  SDL_CloseIO(stream);
  return surface;
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
  Version version;
  if (ParseVersion(DND_VERSION, version)) {
    menu_.version = FormatVersion(version);
    if (version.dirty) {
      menu_.version += " (developer build)";
    }
  } else {
    menu_.version = DND_VERSION;
  }
  menu_.protocol = kProtocolVersion;
}

App::~App() noexcept {
  // Shutdown пишет партию и может не найти память: деструктор не бросает.
  try {
    Shutdown();
  } catch (...) {
    SDL_Log("shutdown failed");
  }
}

auto App::Initialize() -> bool {
  if (!SDL_Init(SDL_INIT_VIDEO)) {
    SDL_Log("SDL_Init failed: %s", SDL_GetError());
    return false;
  }
  window_ = SDL_CreateWindow(std::string(strings::kWindowTitle).c_str(),
                             kWindowWidth, kWindowHeight, SDL_WINDOW_RESIZABLE);
  if (window_ == nullptr) {
    SDL_Log("SDL_CreateWindow failed: %s", SDL_GetError());
    return false;
  }
  if (SDL_Surface* icon = LoadWindowIcon(); icon != nullptr) {
    SDL_SetWindowIcon(window_, icon);
    SDL_DestroySurface(icon);
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

  // Проверка обновлений: developer build молчит, DND_UPDATE_MANIFEST — явный
  // отладочный обход, который разрешает весь цикл без нового релиза.
  Version current_version;
  const bool version_parsed = ParseVersion(DND_VERSION, current_version);
  const char* manifest_env = std::getenv("DND_UPDATE_MANIFEST");
  std::string manifest_override =
      manifest_env == nullptr ? std::string{} : std::string(manifest_env);
  const bool update_enabled =
      !options_.no_update_check && version_parsed &&
      (!current_version.dirty || !manifest_override.empty());
  SDL_Log("update service: enabled=%d, manifest override='%s'",
          update_enabled ? 1 : 0, manifest_override.c_str());
  update_service_ = std::make_unique<UpdateService>(
      BaseDirectory(), current_version, kProtocolVersion, update_enabled,
      std::move(manifest_override));
  update_service_->StartCheck();
  // Отказ по протоколу несёт и версию хоста, и последний релиз: клиент сразу
  // видит, поможет ли ему обновление.
  net_server_.SetMismatchInfoProvider(
      [this]() -> VersionMismatch { return MismatchInfo(); });

  RefreshCampaignLibrary();
  if (!LoadSelectedCampaign(LoadMode::kGame) && campaign_.scenes.empty()) {
    PrepareDefaultCampaign();
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
  // Последние секунды партии не должны пропасть при закрытии окна.
  if (in_game_ && options_.mode == AppMode::kHost && campaign_file_loaded_) {
    StoreActiveScene();
    RequestOverlaySave();
  }
  campaign_saver_.Flush();
  overlay_saver_.Flush();
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
      editor_status_.Set(menu_.status);
    }
    return false;
  }

  // Мир читается до открытия порта: битая партия не должна оставить хост.
  if (in_editor_) {
    // Редактор отдаёт игру из текущей карты, а кампания становится авторским
    // файлом: игра дальше пишет только оверлей.
    if (editor_scene_ < campaign_.scenes.size()) {
      campaign_.scenes[editor_scene_].snapshot = CleanTemplateSnapshot(scene_);
      campaign_.active = editor_scene_;
    }
    RequestCampaignSave();
    if (!campaign_saver_.Flush()) {
      menu_.status = "Не удалось сохранить кампанию";
      editor_status_.Set(menu_.status);
      return false;
    }
    campaign_file_loaded_ = true;
    base_campaign_ = campaign_;
    overlay_ = FreshOverlay(base_campaign_);
    party_error_ = false;
    party_mismatch_ = false;
    RefreshCampaignLibrary();
  } else {
    // Незаписанную партию не бросаем: старт хоста перечитывает диск, поэтому
    // сначала доводим запись до конца.
    if (overlay_write_failed_ && !SaveOverlay()) {
      return false;
    }
    const std::filesystem::path campaign_path =
        campaign_files_.empty() ? AssetPath("campaign.dndcamp")
                                : AssetPath(campaign_files_[campaign_index_]);
    if (!LoadSelectedCampaign(LoadMode::kGame)) {
      if (std::filesystem::exists(campaign_path)) {
        // Кампания или партия не прочитались: стол не стартует.
        if (menu_.status.empty()) {
          menu_.status = "Не удалось прочитать кампанию";
        }
        return false;
      }
      // Пустой слот: первый старт хоста один раз создаёт авторский файл.
      if (!WriteCampaign(campaign_, campaign_path)) {
        menu_.status =
            "Не удалось создать " + campaign_path.filename().string();
        return false;
      }
      campaign_file_loaded_ = true;
      base_campaign_ = campaign_;
      overlay_ = FreshOverlay(base_campaign_);
      party_error_ = false;
      party_mismatch_ = false;
      RefreshCampaignLibrary();
    }
    if (party_error_) {
      menu_.status = std::string(strings::kPartyReadFailed) +
                     OverlayFilePath().filename().string();
      return false;
    }
    if (party_mismatch_) {
      menu_.status = std::string(strings::kPartyMismatchBlocked);
      return false;
    }
  }

  if (campaign_.scenes.empty() || campaign_.active >= campaign_.scenes.size()) {
    menu_.status = "Кампания без сцен";
    return false;
  }
  if (!net_server_.Start(port, menu_.password)) {
    menu_.status = "Не удалось открыть порт " + std::to_string(port);
    if (in_editor_) {
      editor_status_.Set(menu_.status);
    }
    return false;
  }

  options_.mode = AppMode::kHost;
  options_.port = port;
  GameSnapshot start = campaign_.scenes[campaign_.active].snapshot;
  start.encounter = Encounter{};
  scene_ = start;
  server_.Reset(std::move(start));
  server_.ClearClaimHistory();
  ClearUndo();
  delete_token_.Clear();
  new_party_confirm_.Clear();
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
  // Новая партия не должна начинаться с чужого разговора.
  view_.ClearLog();
  players_.clear();
  players_.emplace_back("DM (хост)");
  player_owners_.clear();
  player_owners_.emplace_back();
  local_addresses_ = LocalIpv4Addresses();
  view_.Reset(server_.DmSnapshot(), server_.FullFogState());
  CenterCamera();
  StoreActiveScene();
  SaveOverlay();
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
  claimed_once_ = false;
  in_game_ = false;
  version_mismatch_.reset();
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
  // Недописанную партию не бросаем: вход в редактор перечитывает диск.
  if (overlay_write_failed_ && !SaveOverlay()) {
    return;
  }
  in_editor_ = true;
  in_game_ = false;
  client_connecting_ = false;
  menu_.status.clear();
  // Редактор видит только авторский файл: партия с диска не подмешивается.
  (void)LoadSelectedCampaign(LoadMode::kEditor);
  editor_status_.Clear();
  if (!menu_.status.empty()) {
    editor_status_.Set(menu_.status);
  }
  delete_editor_token_.Clear();
  editor_drag_token_ = 0;
  CancelEditorTokenRename();
  CancelEditorBackgroundEdit();
  CloseSheet();
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
  editor_scenes_status_.Clear();
  editor_scene_edit_ = false;
  editor_scene_edit_buffer_.clear();
  editor_scene_edit_ignore_text_ = false;
  delete_editor_scene_.Clear();
  EndEditorStroke();
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

auto App::ExitToMenu() -> void {
  exit_confirm_active_ = false;
  const bool was_host = options_.mode == AppMode::kHost;
  bool party_saved = true;
  if (in_editor_) {
    campaign_saver_.Flush();
  }
  if (was_host) {
    if (in_game_) {
      // Партия — отдельный файл: кампания при выходе не меняется.
      StoreActiveScene();
      party_saved = SaveOverlay();
    }
    net_server_.Stop();
  } else {
    net_client_.Disconnect();
  }

  in_game_ = false;
  in_editor_ = false;
  client_connecting_ = false;
  client_connected_ = false;
  client_is_dm_ = false;
  claimed_once_ = false;
  menu_.tab = was_host ? MenuTab::kHost : MenuTab::kJoin;
  menu_.active_field = MenuField::kNone;
  menu_.busy = false;
  menu_.status.clear();
  if (!party_saved) {
    // Ошибка записи партии не должна исчезнуть вместе со старым статусом:
    // смена кампании и старт хоста ждут успешного повтора.
    menu_.status = std::string(strings::kPartySaveFailed);
  }

  chat_.Close();
  help_active_ = false;
  history_active_ = false;
  sheet_active_ = false;
  bestiary_active_ = false;
  conditions_active_ = false;
  scenes_active_ = false;
  combat_active_ = false;
  aoe_mode_ = false;
  show_movement_range_ = false;
  measuring_ = false;
  measure_start_.reset();
  measure_end_.reset();
  dragging_ = false;
  drag_target_.reset();
  panning_ = false;
  selected_id_ = 0;
  log_scroll_.ToNewest();
  log_top_ = 0.0F;
  players_.clear();
  player_owners_.clear();
  ClearUndo();
  RefreshCampaignLibrary();
  // Партия не записалась: в памяти её свежая копия, перечитывать диск нельзя.
  if (party_saved) {
    if (!LoadSelectedCampaign(LoadMode::kGame) && campaign_.scenes.empty()) {
      PrepareDefaultCampaign();
    }
  }
  SDL_Log("returned to menu");
}

auto App::SaveScene() -> void {
  if (editor_scene_ >= campaign_.scenes.size()) {
    editor_status_.Set("Сцена не выбрана");
    return;
  }
  // Шаблон карты не несёт следов партии: туман, бой и часы остаются в
  // оверлее, в кампанию и scene.dndscene уходит чистый снимок.
  const GameSnapshot stored = CleanTemplateSnapshot(scene_);
  if (!WriteSnapshot(stored, ScenePath())) {
    editor_status_.Set("Не удалось сохранить сцену");
    return;
  }
  scene_ = stored;
  // F5 пишет ту сцену, которую выбрал DM в панели редактора.
  campaign_.scenes[editor_scene_].snapshot = stored;
  campaign_.active = editor_scene_;
  SaveCampaign();
  editor_status_.Set(std::string(strings::kEditorSceneSaved) + ": " +
                     campaign_.scenes[editor_scene_].name);
}

auto App::LoadScene() -> void {
  const std::optional<GameSnapshot> loaded = ReadSnapshot(ScenePath());
  if (!loaded.has_value()) {
    editor_status_.Set(std::string(strings::kEditorSceneMissing));
    return;
  }
  scene_ = *loaded;
  scene_.encounter = Encounter{};
  selected_id_ = 0;
  dragging_ = false;
  drag_target_.reset();
  delete_editor_token_.Clear();
  editor_drag_token_ = 0;
  CancelEditorTokenRename();
  CancelEditorBackgroundEdit();
  CloseSheet();
  editor_status_.Set(std::string(strings::kEditorSceneLoaded));
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
      if (scene_.map.Contains(cell)) {
        map.SetTerrain(cell, scene_.map.TerrainAt(cell));
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
  editor_status_.Set("Размер карты: " + std::to_string(width) + " x " +
                     std::to_string(height));
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
    editor_status_.Set("Нельзя поставить токен в эту клетку");
    return;
  }
  for (const Token& token : scene_.tokens) {
    if (token.cell == cell) {
      editor_status_.Set("Клетка уже занята токеном");
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

auto App::SpawnEditorBestiary(int index, bool group) -> void {
  const auto& bestiary = DefaultBestiary();
  if (index < 0 || std::cmp_greater_equal(index, bestiary.size())) {
    return;
  }
  const Cell origin = CellFromPixel(mouse_x_, mouse_y_);
  if (!scene_.map.Contains(origin) || scene_.map.IsBlocked(origin)) {
    editor_status_.Set("Нельзя поставить монстра в эту клетку");
    return;
  }
  const MonsterTemplate& monster = bestiary[static_cast<std::size_t>(index)];
  const int count = group ? MonsterGroupSize(rng_) : 1;
  const std::vector<Cell> cells =
      MonsterGroupCells(scene_.map, origin, count, scene_.tokens);
  if (cells.empty()) {
    editor_status_.Set("Нельзя поставить монстра в эту клетку");
    return;
  }
  const bool numbered = cells.size() > 1;
  for (std::size_t i = 0; i < cells.size(); ++i) {
    Token token;
    token.id = scene_.next_token_id++;
    token.name = monster.name;
    if (numbered) {
      token.name += " " + std::to_string(i + 1);
    }
    token.owner = "dm";
    token.hp = monster.max_hp;
    token.max_hp = monster.max_hp;
    token.ac = monster.ac;
    token.speed = monster.speed;
    token.notes = monster.notes;
    token.attack_bonus = monster.attack_bonus;
    token.save_bonus = monster.save_bonus;
    token.damage = monster.damage;
    token.kind = TokenKind::kMonster;
    token.cell = cells[i];
    scene_.tokens.push_back(std::move(token));
  }
  scene_.encounter = Encounter{};
  editor_status_.Set("Добавлен монстр: " + monster.name);
  bestiary_active_ = false;
}

auto App::ApplyEditorBrush(Cell cell) -> void {
  if (!scene_.map.Contains(cell)) {
    return;
  }
  // Кисть не рисует поверх токена: его убирают ПКМ с подтверждением.
  if (EditorTokenAt(cell) != nullptr) {
    return;
  }
  switch (editor_brush_) {
    case EditorBrush::kWall:
      scene_.map.SetTerrain(cell, Terrain::kWall);
      break;
    case EditorBrush::kFloor:
      scene_.map.SetTerrain(cell, Terrain::kFloor);
      break;
    case EditorBrush::kTree:
      scene_.map.SetTerrain(cell, Terrain::kTree);
      break;
    case EditorBrush::kWater:
      scene_.map.SetTerrain(cell, Terrain::kWater);
      break;
    case EditorBrush::kPit:
      scene_.map.SetTerrain(cell, Terrain::kPit);
      break;
    case EditorBrush::kLava:
      scene_.map.SetTerrain(cell, Terrain::kLava);
      break;
  }
  scene_.encounter = Encounter{};
}

auto App::BeginEditorStroke(EditorPaintMode mode, Cell cell) -> void {
  if (mode == EditorPaintMode::kNone) {
    return;
  }
  editor_paint_mode_ = mode;
  editor_stroke_.Begin(cell);
  PaintEditorCell(cell, mode);
}

auto App::ContinueEditorStroke(Cell cell) -> void {
  if (editor_paint_mode_ == EditorPaintMode::kNone) {
    return;
  }
  for (const Cell& painted : editor_stroke_.Move(cell)) {
    PaintEditorCell(painted, editor_paint_mode_);
  }
}

auto App::EndEditorStroke() -> void {
  editor_paint_mode_ = EditorPaintMode::kNone;
  editor_stroke_.End();
}

auto App::PaintEditorCell(Cell cell, EditorPaintMode mode) -> void {
  if (mode == EditorPaintMode::kBrush) {
    ApplyEditorBrush(cell);
  } else if (mode == EditorPaintMode::kErase) {
    EraseEditorCell(cell);
  }
}

auto App::EraseEditorCell(Cell cell) -> void {
  if (!scene_.map.Contains(cell)) {
    return;
  }
  // Штрих ПКМ возвращает полу и не трогает токены: токен под клеткой
  // удаляется только отдельным подтверждённым кликом.
  if (EditorTokenAt(cell) != nullptr) {
    return;
  }
  scene_.map.SetTerrain(cell, Terrain::kFloor);
  scene_.encounter = Encounter{};
}

auto App::EditorTokenAt(Cell cell) const -> const Token* {
  for (const Token& token : scene_.tokens) {
    if (token.cell == cell) {
      return &token;
    }
  }
  return nullptr;
}

auto App::RequestDeleteEditorToken(TokenId id) -> void {
  if (delete_editor_token_.Request(static_cast<std::size_t>(id),
                                   std::chrono::steady_clock::now())) {
    const auto before = scene_.tokens.size();
    std::erase_if(scene_.tokens,
                  [id](const Token& token) -> bool { return token.id == id; });
    if (scene_.tokens.size() != before) {
      if (selected_id_ == id) {
        selected_id_ = 0;
      }
      drag_target_.reset();
      scene_.encounter = Encounter{};
      editor_status_.Set(std::string(strings::kEditorTokenDeleted));
    }
    return;
  }
  editor_status_.Set(std::string(strings::kEditorTokenDeleteConfirm));
}

auto App::FindEditorToken(TokenId id) const -> const Token* {
  if (id == 0) {
    return nullptr;
  }
  for (const Token& token : scene_.tokens) {
    if (token.id == id) {
      return &token;
    }
  }
  return nullptr;
}

auto App::FindEditorToken(TokenId id) -> Token* {
  if (id == 0) {
    return nullptr;
  }
  for (Token& token : scene_.tokens) {
    if (token.id == id) {
      return &token;
    }
  }
  return nullptr;
}

auto App::MoveEditorToken(TokenId id, Cell cell) -> void {
  if (!MoveTokenToCell(scene_.tokens, scene_.map, id, cell)) {
    return;
  }
  scene_.encounter = Encounter{};
  editor_status_.Clear();
}

auto App::BeginEditorTokenRename() -> void {
  if (FindEditorToken(selected_id_) == nullptr) {
    editor_status_.Set(std::string(strings::kSelectToken));
    return;
  }
  editor_token_edit_ = true;
  editor_token_edit_id_ = selected_id_;
  editor_token_edit_buffer_.clear();
  editor_token_edit_ignore_text_ = true;
  editor_status_.Clear();
}

auto App::CancelEditorTokenRename() -> void {
  editor_token_edit_ = false;
  editor_token_edit_id_ = 0;
  editor_token_edit_buffer_.clear();
  editor_token_edit_ignore_text_ = false;
}

auto App::CommitEditorTokenRename() -> void {
  if (!editor_token_edit_) {
    return;
  }
  Token* token = FindEditorToken(editor_token_edit_id_);
  if (token == nullptr) {
    CancelEditorTokenRename();
    return;
  }
  std::string name = TrimTokenName(editor_token_edit_buffer_);
  if (name.empty()) {
    editor_status_.Set(std::string(strings::kEditorTokenNameEmpty));
    return;
  }
  TruncateUtf8(name, kMaxTokenNameLength);
  token->name = std::move(name);
  editor_status_.Set(std::string(strings::kEditorTokenRenamed) + token->name);
  CancelEditorTokenRename();
}

auto App::HandleEditorTokenTextInput(const char* text) -> void {
  if (!editor_token_edit_ || text == nullptr) {
    return;
  }
  // Первый TEXT_INPUT — это сама клавиша R, её не вводим.
  if (editor_token_edit_ignore_text_) {
    editor_token_edit_ignore_text_ = false;
    return;
  }
  AppendUtf8(editor_token_edit_buffer_, text, kMaxTokenNameLength);
  editor_status_.Clear();
}

auto App::HandleEditorTokenEditKeyDown(const SDL_KeyboardEvent& key) -> void {
  if (!editor_token_edit_) {
    return;
  }
  if (key.key == SDLK_ESCAPE) {
    CancelEditorTokenRename();
  } else if (key.key == SDLK_RETURN || key.key == SDLK_KP_ENTER) {
    CommitEditorTokenRename();
  } else if (key.key == SDLK_BACKSPACE) {
    EraseLastCodePoint(editor_token_edit_buffer_);
    editor_status_.Clear();
  }
}

auto App::HandleEditorBackgroundTextInput(const char* text) -> void {
  if (!editor_background_edit_ || text == nullptr) {
    return;
  }
  // Первый TEXT_INPUT — это сама клавиша M, её не вводим.
  if (editor_background_ignore_text_) {
    editor_background_ignore_text_ = false;
    return;
  }
  AppendUtf8(editor_background_buffer_, text, kMaxImageNameLength);
}

auto App::HandleEditorBackgroundKeyDown(const SDL_KeyboardEvent& key) -> void {
  if (!editor_background_edit_) {
    return;
  }
  if (key.key == SDLK_ESCAPE) {
    CancelEditorBackgroundEdit();
    editor_status_.Clear();
  } else if (key.key == SDLK_RETURN || key.key == SDLK_KP_ENTER) {
    CommitEditorBackgroundEdit();
  } else if (key.key == SDLK_BACKSPACE) {
    EraseLastCodePoint(editor_background_buffer_);
  }
}

auto App::BeginEditorBackgroundEdit() -> void {
  editor_background_edit_ = true;
  editor_background_buffer_.clear();
  editor_background_ignore_text_ = true;
  editor_status_.Set(std::string(strings::kEditorBackgroundHint));
}

auto App::CancelEditorBackgroundEdit() -> void {
  editor_background_edit_ = false;
  editor_background_buffer_.clear();
  editor_background_ignore_text_ = false;
}

auto App::CommitEditorBackgroundEdit() -> void {
  if (!editor_background_edit_) {
    return;
  }
  const std::string text(TrimText(editor_background_buffer_));
  MapBackground next = scene_.background;
  if (text.empty() || text == "off") {
    next = MapBackground{};
  } else {
    // Привычную команду «/map ...» принимаем и здесь, с префиксом или без.
    const ChatCommand command =
        ParseChatCommand(text.starts_with("/map") ? text : "/map " + text);
    if (!ApplyMapCommand(next, command)) {
      editor_status_.Set(std::string(strings::kEditorBackgroundInvalid));
      return;
    }
  }
  scene_.background = std::move(next);
  editor_status_.Set(scene_.background.image.empty()
                         ? std::string(strings::kEditorBackgroundCleared)
                         : std::string(strings::kEditorBackgroundSet) +
                               scene_.background.image + " (" +
                               std::to_string(scene_.background.opacity) +
                               "%, " + std::to_string(scene_.background.scale) +
                               "%)");
  CancelEditorBackgroundEdit();
}

auto App::HandleEditorKeyDown(const SDL_KeyboardEvent& key) -> void {
  switch (key.key) {
    case SDLK_ESCAPE:
      CancelEditorTokenRename();
      CancelEditorBackgroundEdit();
      editor_drag_token_ = 0;
      in_editor_ = false;
      menu_.tab = MenuTab::kHost;
      menu_.status.clear();
      RefreshCampaignLibrary();
      // Кампания могла измениться в редакторе: партия сверяется заново.
      if (!LoadSelectedCampaign(LoadMode::kGame) && campaign_.scenes.empty()) {
        PrepareDefaultCampaign();
      }
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
      editor_brush_ = EditorBrush::kTree;
      return;
    case SDLK_4:
      editor_brush_ = EditorBrush::kWater;
      return;
    case SDLK_5:
      editor_brush_ = EditorBrush::kPit;
      return;
    case SDLK_6:
      editor_brush_ = EditorBrush::kLava;
      return;
    case SDLK_P:
      AddEditorToken(CellFromPixel(mouse_x_, mouse_y_), TokenKind::kPlayer);
      return;
    case SDLK_L:
      OpenSheet();
      return;
    case SDLK_R:
      BeginEditorTokenRename();
      return;
    case SDLK_V:
      CycleEditorTokenColor();
      return;
    case SDLK_B:
      OpenBestiary();
      return;
    case SDLK_M:
      BeginEditorBackgroundEdit();
      return;
    case SDLK_HOME:
      CenterCamera();
      return;
    default:
      break;
  }
}

auto App::OpenEditorScenes() -> void {
  EndEditorStroke();
  editor_drag_token_ = 0;
  CancelEditorTokenRename();
  editor_scenes_active_ = true;
  editor_scenes_selected_ = editor_scene_;
  editor_scenes_status_.Clear();
  editor_scene_edit_ = false;
  editor_scene_edit_buffer_.clear();
  editor_scene_edit_ignore_text_ = false;
  delete_editor_scene_.Clear();
}

auto App::CloseEditorScenes() -> void {
  editor_scenes_active_ = false;
  editor_scenes_status_.Clear();
  CancelEditorSceneRename();
  delete_editor_scene_.Clear();
}

auto App::HandleEditorScenesMouseDown(const SDL_MouseButtonEvent& button)
    -> void {
  if (button.button != SDL_BUTTON_LEFT || editor_scene_edit_) {
    return;
  }
  const std::size_t index =
      SceneRowAt(renderer_, text_.get(), campaign_.scenes,
                 editor_scenes_selected_, button.x, button.y);
  if (index == kNoScene) {
    return;
  }
  editor_scenes_selected_ = index;
  editor_scenes_status_.Clear();
  if (button.clicks >= 2) {
    SwitchEditorScene(index);
  }
}

auto App::HandleEditorScenesKeyDown(const SDL_KeyboardEvent& key) -> void {
  if (editor_scene_edit_) {
    if (key.key == SDLK_ESCAPE) {
      CancelEditorSceneRename();
    } else if (key.key == SDLK_RETURN || key.key == SDLK_KP_ENTER) {
      CommitEditorSceneRename();
    } else if (key.key == SDLK_BACKSPACE) {
      EraseLastCodePoint(editor_scene_edit_buffer_);
      editor_scenes_status_.Clear();
    }
    return;
  }
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
    case SDLK_R:
      BeginEditorSceneRename();
      return;
    case SDLK_DELETE:
      RequestDeleteEditorScene();
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

auto App::HandleEditorScenesTextInput(const char* text) -> void {
  if (!editor_scene_edit_ || text == nullptr) {
    return;
  }
  // Первый TEXT_INPUT — это сама клавиша R, её не вводим.
  if (editor_scene_edit_ignore_text_) {
    editor_scene_edit_ignore_text_ = false;
    return;
  }
  AppendUtf8(editor_scene_edit_buffer_, text, kMaxSceneNameLength);
  editor_scenes_status_.Clear();
}

auto App::BeginEditorSceneRename() -> void {
  if (editor_scenes_selected_ >= campaign_.scenes.size()) {
    return;
  }
  editor_scene_edit_ = true;
  // Имя набирается с нуля: старое стирать не нужно.
  editor_scene_edit_buffer_.clear();
  editor_scene_edit_ignore_text_ = true;
  editor_scenes_status_.Clear();
}

auto App::CancelEditorSceneRename() -> void {
  editor_scene_edit_ = false;
  editor_scene_edit_buffer_.clear();
  editor_scene_edit_ignore_text_ = false;
}

auto App::CommitEditorSceneRename() -> void {
  if (!editor_scene_edit_ ||
      editor_scenes_selected_ >= campaign_.scenes.size()) {
    CancelEditorSceneRename();
    return;
  }
  std::string name(TrimText(editor_scene_edit_buffer_));
  if (name.empty()) {
    editor_scenes_status_.Set(std::string(strings::kSceneNameEmpty));
    return;
  }
  TruncateUtf8(name, kMaxSceneNameLength);
  // Проверки те же, что у панели сцен DM: пусто, длина и занятое имя.
  if (!dnd::RenameScene(campaign_, editor_scenes_selected_, std::move(name))) {
    editor_scenes_status_.Set(std::string(strings::kSceneNameTaken));
    return;
  }
  editor_status_.Set(std::string(strings::kEditorSceneRenamed) +
                     campaign_.scenes[editor_scenes_selected_].name);
  CancelEditorSceneRename();
  editor_scenes_status_.Clear();
  SaveCampaign();
}

auto App::RequestDeleteEditorScene() -> void {
  if (editor_scenes_selected_ >= campaign_.scenes.size()) {
    return;
  }
  if (!CanDeleteScene(campaign_)) {
    editor_scenes_status_.Set(std::string(strings::kEditorLastScene));
    delete_editor_scene_.Clear();
    return;
  }
  if (delete_editor_scene_.Request(editor_scenes_selected_,
                                   std::chrono::steady_clock::now())) {
    DeleteEditorScene(editor_scenes_selected_);
    return;
  }
  editor_scenes_status_.Set(std::string(strings::kDeleteConfirm));
}

auto App::DeleteEditorScene(std::size_t index) -> void {
  if (index >= campaign_.scenes.size() || !CanDeleteScene(campaign_)) {
    return;
  }
  EndEditorStroke();
  CancelEditorSceneRename();
  // Правки открытой карты не теряем: она уходит в кампанию до удаления.
  if (editor_scene_ < campaign_.scenes.size()) {
    campaign_.scenes[editor_scene_].snapshot = scene_;
  }
  const std::string name = campaign_.scenes[index].name;
  campaign_.scenes.erase(campaign_.scenes.begin() +
                         static_cast<std::ptrdiff_t>(index));
  const std::size_t next = IndexAfterDelete(index, campaign_.scenes.size());
  const bool deleted_open = index == editor_scene_;
  if (editor_scene_ > index) {
    --editor_scene_;
  }
  if (deleted_open) {
    editor_scene_ = next;
    scene_ = campaign_.scenes[next].snapshot;
    scene_.encounter = Encounter{};
    selected_id_ = 0;
    dragging_ = false;
    drag_target_.reset();
    panning_ = false;
    CenterCamera();
  }
  campaign_.active = editor_scene_;
  editor_scenes_selected_ = next;
  delete_editor_token_.Clear();
  editor_drag_token_ = 0;
  CancelEditorTokenRename();
  CloseSheet();
  editor_scenes_status_.Clear();
  editor_status_.Set(std::string(strings::kEditorSceneDeleted) + name);
  SaveCampaign();
}

auto App::SwitchEditorScene(std::size_t index) -> void {
  if (index >= campaign_.scenes.size()) {
    return;
  }
  EndEditorStroke();
  CancelEditorSceneRename();
  if (index != editor_scene_ && editor_scene_ < campaign_.scenes.size()) {
    // Правки остаются в памяти кампании и сразу уходят на диск: Esc из
    // редактора не должен терять уже закрытую карту.
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
  delete_editor_token_.Clear();
  editor_drag_token_ = 0;
  CancelEditorTokenRename();
  CancelEditorBackgroundEdit();
  CloseSheet();
  CenterCamera();
  SaveCampaign();
  editor_status_.Set(std::string(strings::kEditorSceneOpened) +
                     campaign_.scenes[index].name);
  CloseEditorScenes();
}

auto App::CreateEditorScene() -> void {
  editor_scenes_status_.Clear();
  CancelEditorSceneRename();
  if (campaign_.scenes.size() >= kMaxCampaignScenes) {
    editor_scenes_status_.Set("Достигнут предел числа сцен");
    return;
  }
  if (editor_scene_ < campaign_.scenes.size()) {
    campaign_.scenes[editor_scene_].snapshot = scene_;
  }
  std::string name = NextSceneName(campaign_);
  if (!IsSceneNameAvailable(campaign_, name)) {
    editor_scenes_status_.Set(std::string(strings::kSceneNameTaken));
    return;
  }
  Scene scene{.id = NextSceneId(campaign_),
              .name = std::move(name),
              .snapshot = scene_};
  scene.snapshot.encounter = Encounter{};
  campaign_.scenes.push_back(std::move(scene));
  campaign_.active = campaign_.scenes.size() - 1;
  editor_scene_ = campaign_.active;
  editor_scenes_selected_ = editor_scene_;
  SaveCampaign();
  editor_status_.Set(std::string(strings::kEditorSceneCreated) +
                     campaign_.scenes[editor_scene_].name);
}

auto App::HandleEditorMouseDown(const SDL_MouseButtonEvent& button) -> void {
  const SDL_FRect panel = EditorPanelBounds();
  if (editor_panel_visible_ && PointInside(panel, button.x, button.y)) {
    return;
  }
  if (button.button == SDL_BUTTON_MIDDLE) {
    EndEditorStroke();
    panning_ = true;
    pan_last_x_ = button.x;
    pan_last_y_ = button.y;
    return;
  }
  const Cell cell = CellFromPixel(button.x, button.y);
  if (button.button == SDL_BUTTON_LEFT) {
    // ЛКМ по токену выбирает его и тащит; кисть по токену не рисует.
    if (const Token* token = EditorTokenAt(cell); token != nullptr) {
      selected_id_ = token->id;
      editor_drag_token_ = token->id;
      EndEditorStroke();
      return;
    }
    selected_id_ = 0;
    BeginEditorStroke(EditorPaintMode::kBrush, cell);
  } else if (button.button == SDL_BUTTON_RIGHT) {
    if (const Token* token = EditorTokenAt(cell); token != nullptr) {
      selected_id_ = token->id;
      RequestDeleteEditorToken(token->id);
      return;
    }
    BeginEditorStroke(EditorPaintMode::kErase, cell);
  }
}

auto App::HandleEditorMouseUp(const SDL_MouseButtonEvent& button) -> void {
  // Любая отпущенная кнопка заканчивает штрих, а не только та, что начала.
  editor_drag_token_ = 0;
  EndEditorStroke();
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
  if (version_mismatch_.has_value()) {
    return;  // под диалогом версий поля меню не редактируются
  }
  std::string* buffer = MenuTextBuffer();
  if (buffer == nullptr || text == nullptr) {
    return;
  }
  AppendUtf8(*buffer, text, MenuFieldLimit(menu_.active_field));
}

auto App::HandleMenuKeyDown(const SDL_KeyboardEvent& key) -> void {
  // Диалог версий модальный: остальные клавиши до полей меню не доходят.
  if (version_mismatch_.has_value()) {
    if (key.key == SDLK_ESCAPE) {
      CloseMismatch();
    } else if (key.key == SDLK_RETURN || key.key == SDLK_KP_ENTER) {
      HandleMismatchAction();
    }
    return;
  }
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
    new_party_confirm_.Clear();
    return;
  }
  if (key.key == SDLK_TAB) {
    FocusNextMenuField((key.mod & SDL_KMOD_SHIFT) != 0 ? -1 : 1);
    return;
  }
  if (menu_.active_field == MenuField::kNone &&
      (key.mod & (SDL_KMOD_CTRL | SDL_KMOD_ALT)) == 0) {
    const MenuCommand command =
        MenuCommandForChar(menu_.tab, static_cast<char>(key.key));
    if (command != MenuCommand::kNone) {
      HandleMenuCommand(command);
      return;
    }
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
  const MenuMismatch mismatch = MenuMismatchState();
  if (mismatch.visible) {
    if (!mismatch.action.empty() &&
        PointInside(layout.mismatch_action, button.x, button.y)) {
      HandleMismatchAction();
      return;
    }
    if (PointInside(layout.mismatch_close, button.x, button.y)) {
      CloseMismatch();
    }
    // Клики мимо кнопок в меню не проходят: диалог модальный.
    return;
  }
  const MenuUpdate update = MenuUpdateState();
  if (update.visible) {
    if (PointInside(layout.update_action, button.x, button.y)) {
      if (update.action_enabled) {
        HandleUpdateAction();
      }
      return;
    }
    if (PointInside(layout.update_dismiss, button.x, button.y)) {
      update_dismissed_ = true;
      return;
    }
    if (PointInside(layout.update_banner, button.x, button.y)) {
      return;
    }
  }
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
    if (party_mismatch_ && PointInside(layout.party_line, button.x, button.y)) {
      AcceptParty();
    } else if (PointInside(layout.campaign_line, button.x, button.y)) {
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

auto App::MenuUpdateState() const -> MenuUpdate {
  MenuUpdate update;
  if (update_service_ == nullptr || update_dismissed_) {
    return update;
  }
  const UpdateStatus status = update_service_->Status();
  const std::string version = FormatVersion(status.manifest.version);
  switch (status.stage) {
    case UpdateStage::kUpdateAvailable:
      update.visible = true;
      update.message = std::string(strings::kUpdateAvailable) + version;
      update.action = status.manual_only ? std::string(strings::kUpdateManual)
                                         : std::string(strings::kUpdateAction);
      update.dismiss = std::string(strings::kUpdateLater);
      break;
    case UpdateStage::kDownloading:
      update.visible = true;
      update.message = std::string(strings::kUpdateDownloading);
      if (status.progress > 0) {
        update.message += ": " + std::to_string(status.progress) + "%";
      }
      update.action = std::string(strings::kUpdateBusy);
      update.action_enabled = false;
      update.dismiss = std::string(strings::kUpdateLater);
      break;
    case UpdateStage::kReady:
      update.visible = true;
      update.message = std::string(strings::kUpdateReady) + version;
      update.action = std::string(strings::kUpdateRestart);
      update.dismiss = std::string(strings::kUpdateLater);
      break;
    case UpdateStage::kFailed:
      update.visible = true;
      update.message = status.error.empty()
                           ? std::string(strings::kUpdateFailed)
                           : status.error;
      update.action = std::string(strings::kUpdateRetry);
      update.dismiss = std::string(strings::kUpdateLater);
      break;
    default:
      break;
  }
  return update;
}

auto App::MismatchInfo() const -> VersionMismatch {
  VersionMismatch info;
  info.host_protocol = kProtocolVersion;
  (void)ParseVersion(DND_VERSION, info.host_version);
  if (update_service_ != nullptr) {
    const UpdateStatus status = update_service_->Status();
    // Манифеста ещё нет, пока проверка не дошла до сети: пусто — клиенту
    // честно скажем, что последняя версия неизвестна.
    if (!status.manifest.url.empty()) {
      info.latest_version = status.manifest.version;
      info.latest_protocol =
          static_cast<std::uint32_t>(std::max(status.manifest.protocol, 0));
    }
  }
  return info;
}

auto App::MenuMismatchState() const -> MenuMismatch {
  MenuMismatch dialog;
  if (!version_mismatch_.has_value()) {
    return dialog;
  }
  const VersionMismatch& mismatch = *version_mismatch_;
  Version mine;
  const bool mine_known = ParseVersion(DND_VERSION, mine);
  std::optional<Version> latest = mismatch.latest_version;
  std::uint32_t latest_protocol = mismatch.latest_protocol;
  const UpdateStatus status =
      update_service_ != nullptr ? update_service_->Status() : UpdateStatus{};
  // Если хост не успел проверить манифест, подойдёт и результат своей
  // проверки: версия из релиза у обеих сторон одна и та же.
  if (!latest.has_value() && !status.manifest.url.empty()) {
    latest = status.manifest.version;
    latest_protocol =
        static_cast<std::uint32_t>(std::max(status.manifest.protocol, 0));
  }
  const UpdateAdvice advice =
      mine_known ? AdviseUpdate(mine, mismatch.host_version, latest)
                 : UpdateAdvice::kUnknown;

  dialog.visible = true;
  dialog.title = std::string(strings::kMismatchTitle);
  dialog.mine = std::string(strings::kMismatchMine);
  if (mine_known) {
    dialog.mine += VersionWithProtocol(mine, kProtocolVersion);
  } else {
    dialog.mine += "? (" + std::string(strings::kProtocolLabel) +
                   std::to_string(kProtocolVersion) + ")";
  }
  dialog.host =
      std::string(strings::kMismatchHost) +
      VersionWithProtocol(mismatch.host_version, mismatch.host_protocol);
  dialog.latest = std::string(strings::kMismatchLatest);
  if (latest.has_value()) {
    dialog.latest += VersionWithProtocol(*latest, latest_protocol);
  } else {
    dialog.latest += std::string(strings::kMismatchLatestUnknown);
  }
  dialog.close = std::string(strings::kMismatchClose);

  switch (advice) {
    case UpdateAdvice::kUpdateMine:
      dialog.advice = std::string(strings::kMismatchAdviceUpdateMine);
      if (update_service_ == nullptr || !update_service_->Enabled()) {
        // Отключать обновления умеет developer build: только страница релизов.
        dialog.action = std::string(strings::kMismatchReleases);
        dialog.opens_releases = true;
      } else if (status.manual_only) {
        dialog.action = std::string(strings::kUpdateManual);
      } else if (status.stage == UpdateStage::kReady) {
        dialog.action = std::string(strings::kUpdateRestart);
      } else {
        dialog.action = std::string(strings::kUpdateAction);
      }
      break;
    case UpdateAdvice::kUpdateHost:
      dialog.advice = std::string(strings::kMismatchAdviceUpdateHost);
      break;
    case UpdateAdvice::kNewerAvailable:
      dialog.advice = std::string(strings::kMismatchAdviceNewer);
      if (update_service_ == nullptr || !update_service_->Enabled()) {
        dialog.action = std::string(strings::kMismatchReleases);
        dialog.opens_releases = true;
      } else if (status.manual_only) {
        dialog.action = std::string(strings::kUpdateManual);
      } else if (status.stage == UpdateStage::kReady) {
        dialog.action = std::string(strings::kUpdateRestart);
      } else {
        dialog.action = std::string(strings::kUpdateAction);
      }
      break;
    case UpdateAdvice::kUpToDate:
      dialog.advice = std::string(strings::kMismatchAdviceUpToDate);
      dialog.action = std::string(strings::kMismatchReleases);
      dialog.opens_releases = true;
      break;
    case UpdateAdvice::kUnknown:
      dialog.advice = std::string(strings::kMismatchAdviceUnknown);
      dialog.action = std::string(strings::kMismatchReleases);
      dialog.opens_releases = true;
      break;
  }
  return dialog;
}

auto App::CloseMismatch() -> void { version_mismatch_.reset(); }

auto App::HandleMismatchAction() -> void {
  const MenuMismatch dialog = MenuMismatchState();
  if (!dialog.visible || dialog.action.empty()) {
    return;
  }
  if (dialog.opens_releases) {
    if (!SDL_OpenURL(std::string(kReleasesUrl).c_str())) {
      SDL_Log("SDL_OpenURL failed: %s", SDL_GetError());
    }
    return;
  }
  if (update_service_ == nullptr) {
    return;
  }
  const UpdateStatus status = update_service_->Status();
  if (status.stage == UpdateStage::kReady) {
    ApplyUpdate();
    return;
  }
  if (status.stage == UpdateStage::kUpdateAvailable) {
    // Дальше прогресс показывает обычный баннер меню, диалог уже не нужен.
    version_mismatch_.reset();
    HandleUpdateAction();
    return;
  }
  // Проверка ещё идёт или сорвалась: повторяем, диалог остаётся открытым и
  // сам покажет новый совет.
  update_service_->StartCheck();
}

auto App::SessionUpdateText() const -> std::string {
  if (update_service_ == nullptr || !IsDm()) {
    return {};
  }
  const UpdateStatus status = update_service_->Status();
  if (status.stage != UpdateStage::kUpdateAvailable &&
      status.stage != UpdateStage::kDownloading &&
      status.stage != UpdateStage::kReady) {
    return {};
  }
  return std::string(strings::kUpdateAvailable) +
         FormatVersion(status.manifest.version) +
         std::string(strings::kUpdateSession);
}

auto App::HandleUpdateAction() -> void {
  if (update_service_ == nullptr || in_game_) {
    return;
  }
  const UpdateStatus status = update_service_->Status();
  switch (status.stage) {
    case UpdateStage::kUpdateAvailable:
      if (status.manual_only) {
        // Папка только для чтения: автоматическая подмена невозможна,
        // открываем ссылку на exe в браузере.
        if (!SDL_OpenURL(status.manifest.url.c_str())) {
          SDL_Log("SDL_OpenURL failed: %s", SDL_GetError());
        }
      } else {
        update_service_->StartDownload();
      }
      break;
    case UpdateStage::kReady:
      ApplyUpdate();
      break;
    case UpdateStage::kFailed:
      update_service_->StartCheck();
      break;
    default:
      break;
  }
}

auto App::ApplyUpdate() -> void {
  if (update_service_ == nullptr || in_game_) {
    return;
  }
  if (update_service_->Status().stage != UpdateStage::kReady) {
    return;
  }
  // Окно автосейва не должно потеряться при подмене exe.
  campaign_saver_.Flush();
  overlay_saver_.Flush();
  update_service_->MarkApplying();
  std::string error;
  const bool installed =
      InstallDownloadedUpdate(update_service_->BasePath(), error);
  if (!installed) {
    SDL_Log("update install failed: %s", error.c_str());
    update_service_->Fail(error);
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "virtual_dnd_tabletop",
                             error.c_str(), window_);
    return;
  }
  if (!error.empty()) {
    SDL_Log("update relaunch failed: %s", error.c_str());
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_WARNING, "virtual_dnd_tabletop",
                             error.c_str(), window_);
  }
  SDL_Log("update installed, closing the old process");
  SDL_Event quit{};
  quit.type = SDL_EVENT_QUIT;
  SDL_PushEvent(&quit);
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
  const float requested = kUiScales[static_cast<std::size_t>(ui_scale_index_)];
  // Меню не ужимается: если панель 640x660 с полями не влезает в логический
  // экран, масштаб выше 100% обрезал бы её сверху и снизу.
  constexpr float menu_min_width = 680.0F;
  constexpr float menu_min_height = 720.0F;
  const bool fits = static_cast<float>(width) >= menu_min_width * requested &&
                    static_cast<float>(height) >= menu_min_height * requested;
  const float scale = fits ? requested : 1.0F;
  if (!fits) {
    SDL_Log("ui scale %.0f%% does not fit the window; keeping 100%%",
            requested * 100.0F);
  }
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
      // У DejaVu тесная строка: обычный хинтинг делает выносные элементы
      // выше расчётной строки, и SDL_ttf обрезает их снизу (видно в release).
      // Небольшой запас строки сохраняет хинтинг и возвращает «у» и «р».
      TTF_SetFontLineSkip(font_, TTF_GetFontHeight(font_) + 2);
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
  // Подтверждение выхода — верхняя модальная панель стола: пока она открыта,
  // остальной ввод не проходит.
  if (exit_confirm_active_) {
    if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat) {
      if (event.key.key == SDLK_RETURN || event.key.key == SDLK_KP_ENTER) {
        ExitToMenu();
      } else if (event.key.key == SDLK_ESCAPE) {
        exit_confirm_active_ = false;
      }
    }
    return;
  }
  // Справку ловим раньше остальных панелей: F1 должен открываться всегда,
  // даже поверх листа персонажа или строки чата.
  if (in_game_ && !in_editor_ && event.type == SDL_EVENT_KEY_DOWN &&
      event.key.key == SDLK_F1 && !event.key.repeat) {
    help_active_ = !help_active_;
    if (help_active_) {
      history_active_ = false;
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

  // Журнал стола — тоже модальная панель: F3 открывает и закрывает её в любом
  // режиме стола, а пока она открыта, ввод идёт в поиск.
  if (in_game_ && !in_editor_ && event.type == SDL_EVENT_KEY_DOWN &&
      event.key.key == SDLK_F3 && !event.key.repeat) {
    if (history_active_) {
      CloseLogHistory();
    } else {
      OpenLogHistory();
    }
    return;
  }
  if (history_active_ && in_game_ && !in_editor_) {
    switch (event.type) {
      case SDL_EVENT_TEXT_INPUT:
        HandleLogHistoryTextInput(event.text.text);
        break;
      case SDL_EVENT_KEY_DOWN:
        HandleLogHistoryKeyDown(event.key);
        break;
      case SDL_EVENT_MOUSE_WHEEL:
        history_scroll_.SetVisibleLines(
            LogHistoryVisibleRows(renderer_, text_.get()));
        history_scroll_.Scroll(static_cast<int>(event.wheel.y * 3.0F),
                               history_matches_.size());
        break;
      default:
        break;
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
      case SDL_EVENT_TEXT_INPUT:
        HandleConditionsTextInput(event.text.text);
        break;
      case SDL_EVENT_KEY_DOWN:
        HandleConditionsKeyDown(event.key);
        break;
      default:
        break;
    }
    return;
  }

  if (actions_active_) {
    if (event.type == SDL_EVENT_KEY_DOWN) {
      HandleActionsKeyDown(event.key);
    }
    return;
  }

  if (combat_active_) {
    if (event.type == SDL_EVENT_KEY_DOWN) {
      HandleCombatKeyDown(event.key);
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
    if (editor_background_edit_) {
      switch (event.type) {
        case SDL_EVENT_TEXT_INPUT:
          HandleEditorBackgroundTextInput(event.text.text);
          break;
        case SDL_EVENT_KEY_DOWN:
          HandleEditorBackgroundKeyDown(event.key);
          break;
        default:
          break;
      }
      return;
    }
    if (editor_token_edit_) {
      switch (event.type) {
        case SDL_EVENT_TEXT_INPUT:
          HandleEditorTokenTextInput(event.text.text);
          break;
        case SDL_EVENT_KEY_DOWN:
          HandleEditorTokenEditKeyDown(event.key);
          break;
        default:
          break;
      }
      return;
    }
    if (editor_scenes_active_) {
      switch (event.type) {
        case SDL_EVENT_TEXT_INPUT:
          HandleEditorScenesTextInput(event.text.text);
          break;
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
        mouse_x_ = event.motion.x;
        mouse_y_ = event.motion.y;
        HandlePanMotion(event.motion);
        if (panning_) {
          EndEditorStroke();
        } else if (editor_drag_token_ != 0) {
          MoveEditorToken(editor_drag_token_,
                          CellFromPixel(event.motion.x, event.motion.y));
        } else {
          ContinueEditorStroke(CellFromPixel(event.motion.x, event.motion.y));
        }
        break;
      case SDL_EVENT_MOUSE_WHEEL:
        HandleWheel(event.wheel);
        break;
      case SDL_EVENT_KEY_DOWN:
        HandleEditorKeyDown(event.key);
        break;
      case SDL_EVENT_WINDOW_FOCUS_LOST:
        // Потеря фокуса отпускает кнопку мыши, не дожидаясь MOUSE_UP.
        editor_drag_token_ = 0;
        EndEditorStroke();
        panning_ = false;
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
      // Сначала Esc снимает активный режим, и только свободный Esc спрашивает
      // подтверждение выхода: случайное нажатие не рвёт сессию.
      const bool had_mode = dragging_ || measure_start_.has_value() ||
                            measure_end_.has_value() || aoe_mode_ ||
                            view_.Aoe().shape != AoeShape::kNone;
      dragging_ = false;
      drag_target_.reset();
      measuring_ = false;
      measure_start_.reset();
      measure_end_.reset();
      ClearAoe();
      if (!had_mode) {
        exit_confirm_active_ = true;
      }
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
    case SDLK_X:
      OpenActions();
      break;
    case SDLK_M:
      if (FindSelectedToken() == nullptr) {
        view_.AddLocalLine(std::string(strings::kSelectToken));
      } else {
        show_movement_range_ = !show_movement_range_;
      }
      break;
    case SDLK_J:
      JumpSelectedToken();
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
      AttackTarget((key.mod & SDL_KMOD_SHIFT) != 0,
                   (key.mod & SDL_KMOD_CTRL) != 0);
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
    case SDLK_T:
      if ((key.mod & SDL_KMOD_SHIFT) != 0) {
        OpenCombat();
      } else if (options_.mode == AppMode::kHost) {
        if (view_.EncounterState().active) {
          SendCommand(EndEncounter{});
        } else {
          SendCommand(StartEncounter{});
        }
      }
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
    case SDLK_G:
      SendCommand(NextTurn{});
      break;
    default:
      break;
  }
}

auto App::HandleMouseDown(const SDL_MouseButtonEvent& button) -> void {
  // Панели списков рисуются поверх карты и первыми забирают клик.
  const TokenId listed =
      view_.EncounterState().active
          ? InitiativeTokenAt(renderer_, text_.get(), view_.Tokens(),
                              view_.EncounterState(), button.x, button.y)
          : 0;
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
      // Ctrl+перетаскивание — ползание, если токен «сбит с ног».
      SubmitMove(cell, (SDL_GetModState() & SDL_KMOD_CTRL) != 0);
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

auto App::SubmitMove(Cell target, bool crawl, bool jump) -> void {
  SendCommand(MoveToken{
      .id = selected_id_, .target = target, .crawl = crawl, .jump = jump});
}

auto App::JumpSelectedToken() -> void {
  const Token* token = FindSelectedToken();
  if (token == nullptr) {
    view_.AddLocalLine(std::string(strings::kSelectToken));
    return;
  }
  const Cell target = CellFromPixel(mouse_x_, mouse_y_);
  if (!view_.Map().Contains(target)) {
    view_.AddLocalLine("Клетка прыжка за пределами карты");
    return;
  }
  if (target == token->cell) {
    view_.AddLocalLine("Наведите курсор на клетку прыжка");
    return;
  }
  SubmitMove(target, false, true);
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

auto App::BottomHintText() const -> std::string {
  const bool is_client = options_.mode == AppMode::kClient;
  const Token* selected = FindSelectedToken();
  HintContext context;
  context.is_dm = !is_client;
  context.has_selection = selected != nullptr;
  context.can_view_sheet = selected != nullptr && CanOpenSheet(*selected);
  context.can_claim = is_client && selected != nullptr &&
                      selected->kind == TokenKind::kPlayer &&
                      selected->owner.empty() && !PlayerHasCharacter() &&
                      !claimed_once_;
  context.in_combat = view_.EncounterState().active;
  context.log_scrolled = log_scroll_.Offset() > 0;
  context.prone = selected != nullptr && IsProne(*selected);
  // Открытый режим важнее выделения: пока он включён, остальные клавиши
  // молчат, поэтому и подсказка показывает только его.
  if (aoe_mode_) {
    context.mode = HintMode::kAoe;
  } else if (sheet_active_) {
    context.mode = HintMode::kSheet;
  } else if (bestiary_active_) {
    context.mode = HintMode::kBestiary;
  } else if (scenes_active_) {
    context.mode = HintMode::kScenes;
  } else if (conditions_active_) {
    context.mode = HintMode::kConditions;
  } else if (actions_active_) {
    context.mode = HintMode::kActions;
  } else if (combat_active_) {
    context.mode = HintMode::kCombat;
  }
  std::string text;
  for (const Hint hint : BottomHints(context)) {
    if (!text.empty()) {
      text += " | ";
    }
    text += strings::HintLabel(hint);
  }
  return text;
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
    view_.AddLocalLine("У вас уже есть персонаж");
    return;
  }
  // Выбор даётся один раз: после /owner прежний игрок сам не забирает.
  if (!SamePlayerName(token->owner, options_.player_name) && claimed_once_) {
    view_.AddLocalLine(std::string(strings::kClaimOnce));
    return;
  }
  SendCommand(ClaimToken{.id = token->id});
}

auto App::ReleaseSelectedToken() -> void {
  // Освобождать персонажа может только DM: у игрока от этого пропадал обзор.
  if (!IsDm()) {
    view_.AddLocalLine(std::string(strings::kReleaseDmOnly));
    return;
  }
  const Token* token = FindSelectedToken();
  if (token == nullptr) {
    view_.AddLocalLine(std::string(strings::kSelectToken));
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

auto App::CanOpenSheet(const Token& token) const -> bool {
  return token.kind == TokenKind::kPlayer || CanEditToken(token);
}

auto App::IsDm() const -> bool {
  return options_.mode == AppMode::kHost || client_is_dm_;
}

auto App::OpenSheet() -> void {
  const Token* token =
      in_editor_ ? FindEditorToken(selected_id_) : FindSelectedToken();
  if (token == nullptr) {
    ShowStatus(std::string(strings::kSelectToken));
    return;
  }
  // Чужой лист персонажа открыт на просмотр: правит листы только DM.
  // В редакторе лист всегда правит DM.
  if (!in_editor_ && !CanOpenSheet(*token)) {
    ShowStatus(std::string(strings::kSheetNoAccess));
    return;
  }
  sheet_active_ = true;
  sheet_read_only_ = !in_editor_ && !IsDm();
  conditions_active_ = false;
  conditions_read_only_ = false;
  bestiary_active_ = false;
  actions_active_ = false;
  combat_active_ = false;
  editor_token_edit_ = false;
  editor_token_edit_buffer_.clear();
  editor_token_edit_ignore_text_ = false;
  sheet_token_id_ = token->id;
  sheet_field_ = SheetField::kAc;
  LoadSheetFields(*token);
}

auto App::LoadSheetFields(const Token& token) -> void {
  sheet_ac_ = std::to_string(token.ac);
  sheet_speed_ = std::to_string(token.speed);
  sheet_attack_ = std::to_string(token.attack_bonus);
  sheet_save_ = std::to_string(token.save_bonus);
  sheet_initiative_ = std::to_string(token.initiative_bonus);
  sheet_temp_hp_ = std::to_string(token.temp_hp);
  sheet_hit_die_ = std::to_string(token.hit_die);
  sheet_con_ = std::to_string(token.con_modifier);
  sheet_range_ = std::to_string(token.attack_range);
  sheet_damage_ = token.damage;
  sheet_resist_ = FormatDamageTypeList(token.resist);
  sheet_vulnerable_ = FormatDamageTypeList(token.vulnerable);
  sheet_immune_ = FormatDamageTypeList(token.immune);
  sheet_notes_ = token.notes;
}

auto App::ShowStatus(std::string_view message) -> void {
  if (in_editor_) {
    editor_status_.Set(std::string(message));
    return;
  }
  view_.AddLocalLine(std::string(message));
}

auto App::DrawActiveSheet() -> void {
  if (!sheet_active_) {
    return;
  }
  const Token* token = nullptr;
  if (in_editor_) {
    token = FindEditorToken(sheet_token_id_);
  } else {
    for (const Token& candidate : view_.Tokens()) {
      if (candidate.id == sheet_token_id_) {
        token = &candidate;
        break;
      }
    }
  }
  if (token == nullptr) {
    CloseSheet();
    return;
  }
  DrawSheetPanel(renderer_, text_.get(), *token, sheet_field_, sheet_ac_,
                 sheet_speed_, sheet_attack_, sheet_save_, sheet_initiative_,
                 sheet_temp_hp_, sheet_hit_die_, sheet_con_, sheet_range_,
                 sheet_damage_, sheet_resist_, sheet_vulnerable_, sheet_immune_,
                 sheet_notes_, sheet_read_only_);
}

auto App::CloseSheet() -> void {
  sheet_active_ = false;
  sheet_read_only_ = false;
  sheet_token_id_ = 0;
}

auto App::SaveSheet() -> void {
  if (sheet_read_only_) {
    CloseSheet();
    return;
  }
  SheetValues values;
  if (!ReadSheetValues(values)) {
    return;
  }
  if (in_editor_) {
    Token* token = FindEditorToken(sheet_token_id_);
    if (token == nullptr) {
      CloseSheet();
      return;
    }
    token->ac = values.ac;
    token->speed = values.speed;
    token->notes = std::move(values.notes);
    token->attack_bonus = values.attack;
    token->save_bonus = values.save;
    token->initiative_bonus = values.initiative;
    token->damage = std::move(values.damage);
    // Как и в игре, временные HP не складываются: остаётся большее.
    token->temp_hp = std::max(token->temp_hp, values.temp_hp);
    token->hit_die = values.hit_die;
    token->con_modifier = values.con;
    token->attack_range = values.attack_range;
    token->resist = values.resist & kAllDamageTypes;
    token->vulnerable = values.vulnerable & kAllDamageTypes;
    token->immune = values.immune & kAllDamageTypes;
    editor_status_.Set(std::string(strings::kEditorSheetSaved) + token->name);
    CloseSheet();
    return;
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
                               .ac = values.ac,
                               .speed = values.speed,
                               .notes = std::move(values.notes),
                               .attack_bonus = values.attack,
                               .save_bonus = values.save,
                               .initiative_bonus = values.initiative,
                               .damage = std::move(values.damage),
                               .temp_hp = values.temp_hp,
                               .hit_die = values.hit_die,
                               .con_modifier = values.con,
                               .attack_range = values.attack_range,
                               .resist = values.resist,
                               .vulnerable = values.vulnerable,
                               .immune = values.immune});
  CloseSheet();
}

auto App::ReadSheetValues(SheetValues& values) -> bool {
  if (!ParseNumber(sheet_ac_, values.ac) || values.ac > 1000 ||
      !ParseSignedNumber(sheet_speed_, values.speed) || values.speed < 0 ||
      values.speed > 1000 || !ParseSignedNumber(sheet_attack_, values.attack) ||
      values.attack < -100 || values.attack > 100 ||
      !ParseSignedNumber(sheet_save_, values.save) || values.save < -100 ||
      values.save > 100 ||
      !ParseSignedNumber(sheet_initiative_, values.initiative) ||
      values.initiative < -100 || values.initiative > 100 ||
      !ParseSignedNumber(sheet_temp_hp_, values.temp_hp) ||
      values.temp_hp < 0 || values.temp_hp > 1000) {
    ShowStatus("Некорректные значения полей листа");
    return false;
  }
  if (!ParseNumber(sheet_hit_die_, values.hit_die) ||
      !IsValidHitDie(values.hit_die)) {
    ShowStatus(std::string(strings::kSheetBadHitDie));
    return false;
  }
  if (!ParseSignedNumber(sheet_con_, values.con) ||
      values.con < -kMaxConModifier || values.con > kMaxConModifier) {
    ShowStatus(std::string(strings::kSheetBadCon));
    return false;
  }
  if (!ParseSignedNumber(sheet_range_, values.attack_range) ||
      values.attack_range < 0 || values.attack_range > 1000) {
    ShowStatus(std::string(strings::kSheetBadRange));
    return false;
  }
  if (!sheet_damage_.empty()) {
    DiceExpression damage;
    DamageType damage_type = DamageType::kNone;
    if (!ParseDamageProfile(sheet_damage_, damage, damage_type)) {
      ShowStatus(std::string(strings::kSheetBadDamage));
      return false;
    }
  }
  if (!ParseDamageTypeList(sheet_resist_, values.resist) ||
      !ParseDamageTypeList(sheet_vulnerable_, values.vulnerable) ||
      !ParseDamageTypeList(sheet_immune_, values.immune)) {
    ShowStatus(std::string(strings::kSheetBadDamageType));
    return false;
  }
  values.damage = sheet_damage_;
  values.notes = sheet_notes_;
  return true;
}

auto App::AttackTarget(bool advantage, bool disadvantage) -> void {
  const Token* attacker = FindSelectedToken();
  if (attacker == nullptr) {
    view_.AddLocalLine(std::string(strings::kSelectToken));
    return;
  }
  if (advantage && disadvantage) {
    view_.AddLocalLine(std::string(strings::kAttackModes));
    return;
  }
  const Cell cell = CellFromPixel(mouse_x_, mouse_y_);
  const TokenId target_id = TokenAt(cell);
  if (target_id == 0) {
    view_.AddLocalLine(std::string(strings::kAttackNoTarget));
    return;
  }
  if (target_id == attacker->id) {
    view_.AddLocalLine(std::string(strings::kAttackSelf));
    return;
  }
  // Хост — DM и не ограничен ходом. Клиент в чужой ход отправляет атаку как
  // реакцию: сервер проверит запас реакции и соседнюю цель.
  const Encounter& encounter = view_.EncounterState();
  const bool own_turn = !encounter.active ||
                        (encounter.turn_index < encounter.order.size() &&
                         encounter.order[encounter.turn_index] == attacker->id);
  const bool reaction = options_.mode == AppMode::kClient && !own_turn;
  SendCommand(AttackToken{.attacker = attacker->id,
                          .target = target_id,
                          .advantage = advantage,
                          .disadvantage = disadvantage,
                          .reaction = reaction});
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
  if (text == nullptr || sheet_read_only_) {
    return;
  }
  if (sheet_field_ == SheetField::kAc || sheet_field_ == SheetField::kSpeed ||
      sheet_field_ == SheetField::kAttack ||
      sheet_field_ == SheetField::kSave ||
      sheet_field_ == SheetField::kInitiative ||
      sheet_field_ == SheetField::kTempHp ||
      sheet_field_ == SheetField::kHitDie || sheet_field_ == SheetField::kCon ||
      sheet_field_ == SheetField::kRange) {
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
      case SheetField::kHitDie:
        AppendBoundedNumber(sheet_hit_die_, text, 1, 20, false);
        break;
      case SheetField::kCon:
        AppendBoundedNumber(sheet_con_, text, -kMaxConModifier, kMaxConModifier,
                            true);
        break;
      case SheetField::kRange:
        AppendBoundedNumber(sheet_range_, text, 0, 1000, false);
        break;
      default:
        break;
    }
    return;
  }
  if (sheet_field_ == SheetField::kDamage) {
    std::string candidate = sheet_damage_;
    candidate += text;
    if (IsDamageProfilePrefix(candidate) && candidate.size() <= 64) {
      sheet_damage_ = std::move(candidate);
    }
    return;
  }
  if (sheet_field_ == SheetField::kResist) {
    AppendUtf8(sheet_resist_, text, 128);
    return;
  }
  if (sheet_field_ == SheetField::kVulnerable) {
    AppendUtf8(sheet_vulnerable_, text, 128);
    return;
  }
  if (sheet_field_ == SheetField::kImmune) {
    AppendUtf8(sheet_immune_, text, 128);
    return;
  }
  AppendUtf8(sheet_notes_, text, 512);
}

auto App::HandleSheetKeyDown(const SDL_KeyboardEvent& key) -> void {
  if (key.key == SDLK_ESCAPE) {
    CloseSheet();
    return;
  }
  if (sheet_read_only_) {
    return;
  }
  if (key.key == SDLK_RETURN || key.key == SDLK_KP_ENTER) {
    SaveSheet();
    return;
  }
  if (key.key == SDLK_TAB) {
    const int next = (static_cast<int>(sheet_field_) + 1) % kSheetFieldCount;
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
    case SheetField::kHitDie:
      buffer = &sheet_hit_die_;
      break;
    case SheetField::kCon:
      buffer = &sheet_con_;
      break;
    case SheetField::kRange:
      buffer = &sheet_range_;
      break;
    case SheetField::kDamage:
      buffer = &sheet_damage_;
      break;
    case SheetField::kResist:
      buffer = &sheet_resist_;
      break;
    case SheetField::kVulnerable:
      buffer = &sheet_vulnerable_;
      break;
    case SheetField::kImmune:
      buffer = &sheet_immune_;
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
  if (!in_editor_ && options_.mode != AppMode::kHost) {
    view_.AddLocalLine(std::string(strings::kBestiaryDmOnly));
    return;
  }
  bestiary_active_ = true;
  sheet_active_ = false;
  sheet_read_only_ = false;
  conditions_active_ = false;
  conditions_read_only_ = false;
  actions_active_ = false;
  combat_active_ = false;
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
  if (in_editor_) {
    SpawnEditorBestiary(index, group);
    return;
  }
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
        .attack_range = monster.attack_range,
        .resist = monster.resist,
        .vulnerable = monster.vulnerable,
        .immune = monster.immune,
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
  conditions_active_ = true;
  conditions_read_only_ = !IsDm();
  conditions_token_id_ = token->id;
  conditions_draft_ = token->conditions;
  conditions_selected_ = 0;
  conditions_dc_edit_ = false;
  conditions_dc_buffer_.clear();
  conditions_custom_edit_ = false;
  conditions_custom_buffer_.clear();
  conditions_custom_ignore_text_ = false;
  const auto& standard = DefaultConditions();
  for (std::size_t i = 0; i < standard.size(); ++i) {
    if (std::ranges::any_of(conditions_draft_,
                            [&standard, i](const Condition& item) -> bool {
                              return item.name == standard[i];
                            })) {
      conditions_selected_ = i;
      break;
    }
  }
  sheet_active_ = false;
  sheet_read_only_ = false;
  bestiary_active_ = false;
  actions_active_ = false;
}

auto App::CloseConditions() -> void {
  conditions_active_ = false;
  conditions_read_only_ = false;
  conditions_dc_edit_ = false;
  conditions_dc_buffer_.clear();
  conditions_custom_edit_ = false;
  conditions_custom_buffer_.clear();
  conditions_custom_ignore_text_ = false;
  conditions_token_id_ = 0;
  conditions_draft_.clear();
}

auto App::OpenActions() -> void {
  const Token* token = FindSelectedToken();
  if (token == nullptr) {
    view_.AddLocalLine(std::string(strings::kSelectToken));
    return;
  }
  if (!CanEditToken(*token)) {
    view_.AddLocalLine(std::string(strings::kActionsNoAccess));
    return;
  }
  actions_active_ = true;
  actions_token_id_ = token->id;
  sheet_active_ = false;
  sheet_read_only_ = false;
  conditions_active_ = false;
  bestiary_active_ = false;
}

auto App::CloseActions() -> void {
  actions_active_ = false;
  actions_token_id_ = 0;
}

auto App::HandleActionsKeyDown(const SDL_KeyboardEvent& key) -> void {
  if (key.repeat) {
    return;
  }
  if (key.key == SDLK_ESCAPE) {
    CloseActions();
    return;
  }
  if (key.key >= SDLK_1 &&
      key.key < SDLK_1 + static_cast<SDL_Keycode>(kTurnActionCount)) {
    const auto index = static_cast<std::uint8_t>(key.key - SDLK_1);
    SendCommand(UseTurnAction{.id = actions_token_id_,
                              .kind = static_cast<TurnActionKind>(index)});
    CloseActions();
  }
}

auto App::OpenCombat() -> void {
  if (options_.mode != AppMode::kHost) {
    view_.AddLocalLine(std::string(strings::kCombatDmOnly));
    return;
  }
  if (!view_.EncounterState().active) {
    view_.AddLocalLine(std::string(strings::kCombatNoEncounter));
    return;
  }
  if (view_.Tokens().empty()) {
    return;
  }
  combat_active_ = true;
  // Курсор панели сразу на текущем ходу: DM чаще правит его состав.
  combat_selected_ = 0;
  const Encounter& encounter = view_.EncounterState();
  const TokenId active = encounter.turn_index < encounter.order.size()
                             ? encounter.order[encounter.turn_index]
                             : 0;
  const std::vector<Token>& tokens = view_.Tokens();
  for (std::size_t i = 0; i < tokens.size(); ++i) {
    if (tokens[i].id == active) {
      combat_selected_ = i;
      break;
    }
  }
  sheet_active_ = false;
  sheet_read_only_ = false;
  conditions_active_ = false;
  actions_active_ = false;
  bestiary_active_ = false;
}

auto App::CloseCombat() -> void {
  combat_active_ = false;
  combat_selected_ = 0;
}

auto App::ToggleCombatParticipant() -> void {
  const std::vector<Token>& tokens = view_.Tokens();
  if (!combat_active_ || combat_selected_ >= tokens.size()) {
    return;
  }
  const Token& token = tokens[combat_selected_];
  const Encounter& encounter = view_.EncounterState();
  const bool joined =
      std::ranges::find(encounter.order, token.id) != encounter.order.end();
  SendCommand(SetEncounterComposition{.id = token.id, .joined = !joined});
}

auto App::HandleCombatKeyDown(const SDL_KeyboardEvent& key) -> void {
  if (key.repeat && key.key != SDLK_UP && key.key != SDLK_DOWN) {
    return;
  }
  if (key.key == SDLK_ESCAPE || key.key == SDLK_T) {
    CloseCombat();
    return;
  }
  const std::size_t count = view_.Tokens().size();
  if (count == 0) {
    CloseCombat();
    return;
  }
  if (key.key == SDLK_UP) {
    combat_selected_ = (combat_selected_ + count - 1) % count;
    return;
  }
  if (key.key == SDLK_DOWN) {
    combat_selected_ = (combat_selected_ + 1) % count;
    return;
  }
  if (key.key == SDLK_SPACE || key.key == SDLK_RETURN ||
      key.key == SDLK_KP_ENTER) {
    ToggleCombatParticipant();
  }
}

auto App::ToggleCondition(int index, bool until_removed) -> void {
  if (conditions_read_only_) {
    return;
  }
  const auto& standard = DefaultConditions();
  if (std::cmp_greater_equal(index, standard.size())) {
    return;
  }
  conditions_selected_ = static_cast<std::size_t>(index);
  const std::string& condition = standard[static_cast<std::size_t>(index)];
  if (condition == kExhaustion) {
    // Каждое включение истощения добавляет уровень, выше шестого нельзя.
    if (ExhaustionLevel(conditions_draft_) >= kMaxExhaustionLevel) {
      view_.AddLocalLine(std::string(strings::kConditionExhaustionMax));
      return;
    }
    conditions_draft_.emplace_back(condition);
    SendCommand(UpdateTokenCombat{.id = conditions_token_id_,
                                  .conditions = conditions_draft_});
    return;
  }
  const auto it = std::ranges::find_if(
      conditions_draft_, [&condition](const Condition& item) -> bool {
        return item.name == condition;
      });
  if (it == conditions_draft_.end()) {
    conditions_draft_.emplace_back(
        condition, until_removed ? 0 : kDefaultConditionSeconds);
  } else {
    conditions_draft_.erase(it);
  }
  SendCommand(UpdateTokenCombat{.id = conditions_token_id_,
                                .conditions = conditions_draft_});
}

auto App::RemoveExhaustionLevelFromDraft() -> void {
  if (conditions_read_only_) {
    return;
  }
  const auto it = std::ranges::find_if(
      conditions_draft_,
      [](const Condition& item) -> bool { return item.name == kExhaustion; });
  if (it == conditions_draft_.end()) {
    return;
  }
  conditions_draft_.erase(it);
  SendCommand(UpdateTokenCombat{.id = conditions_token_id_,
                                .conditions = conditions_draft_});
}

auto App::ApplyConditionCustom(bool until_removed) -> void {
  std::string name = conditions_custom_buffer_;
  while (!name.empty() && name.front() == ' ') {
    name.erase(name.begin());
  }
  while (!name.empty() && name.back() == ' ') {
    name.pop_back();
  }
  TruncateUtf8(name, kMaxConditionNameBytes);
  conditions_custom_edit_ = false;
  conditions_custom_buffer_.clear();
  if (name.empty() || conditions_read_only_) {
    return;
  }
  const auto exists = std::ranges::any_of(
      conditions_draft_,
      [&name](const Condition& item) -> bool { return item.name == name; });
  if (!exists) {
    conditions_draft_.emplace_back(
        name, until_removed ? 0 : kDefaultConditionSeconds);
  }
  SendCommand(UpdateTokenCombat{.id = conditions_token_id_,
                                .conditions = conditions_draft_});
}

auto App::HandleConditionsKeyDown(const SDL_KeyboardEvent& key) -> void {
  if (conditions_dc_edit_ || conditions_custom_edit_) {
    if (key.key == SDLK_ESCAPE) {
      conditions_dc_edit_ = false;
      conditions_dc_buffer_.clear();
      conditions_custom_edit_ = false;
      conditions_custom_buffer_.clear();
      return;
    }
    if (key.key == SDLK_RETURN || key.key == SDLK_KP_ENTER) {
      if (conditions_dc_edit_) {
        ApplyConditionDc();
      } else {
        ApplyConditionCustom((key.mod & SDL_KMOD_SHIFT) != 0);
      }
      return;
    }
    if (key.key == SDLK_BACKSPACE) {
      EraseLastCodePoint(conditions_dc_edit_ ? conditions_dc_buffer_
                                             : conditions_custom_buffer_);
      return;
    }
    return;
  }
  if (key.key == SDLK_ESCAPE || key.key == SDLK_K) {
    CloseConditions();
    return;
  }
  if (conditions_read_only_) {
    return;
  }
  const std::size_t count = DefaultConditions().size();
  if (key.key == SDLK_UP && count > 0) {
    conditions_selected_ = (conditions_selected_ + count - 1) % count;
    return;
  }
  if (key.key == SDLK_DOWN && count > 0) {
    conditions_selected_ = (conditions_selected_ + 1) % count;
    return;
  }
  if (key.key == SDLK_SPACE || key.key == SDLK_RETURN ||
      key.key == SDLK_KP_ENTER) {
    ToggleCondition(static_cast<int>(conditions_selected_),
                    (key.mod & SDL_KMOD_SHIFT) != 0);
    return;
  }
  if (key.key == SDLK_T) {
    conditions_custom_edit_ = true;
    conditions_custom_buffer_.clear();
    conditions_custom_ignore_text_ = true;
    return;
  }
  if (key.key == SDLK_MINUS || key.key == SDLK_KP_MINUS) {
    RemoveExhaustionLevelFromDraft();
    return;
  }
  if (key.key == SDLK_C) {
    const auto& standard = DefaultConditions();
    if (std::cmp_greater_equal(conditions_selected_, standard.size())) {
      return;
    }
    const std::string& name = standard[conditions_selected_];
    const auto it = std::ranges::find_if(
        conditions_draft_,
        [&name](const Condition& item) -> bool { return item.name == name; });
    if (it == conditions_draft_.end()) {
      view_.AddLocalLine(std::string(strings::kConditionDcNeedActive));
      return;
    }
    conditions_dc_edit_ = true;
    conditions_dc_buffer_ = it->save_dc > 0 ? std::to_string(it->save_dc) : "";
    return;
  }
  if (key.key == SDLK_0) {
    conditions_draft_.clear();
    conditions_dc_edit_ = false;
    conditions_dc_buffer_.clear();
    SendCommand(UpdateTokenCombat{.id = conditions_token_id_,
                                  .conditions = conditions_draft_});
    return;
  }
  if (key.key >= SDLK_1 && key.key <= SDLK_9) {
    ToggleCondition(static_cast<int>(key.key - SDLK_1),
                    (key.mod & SDL_KMOD_SHIFT) != 0);
  }
}

auto App::HandleConditionsTextInput(const char* text) -> void {
  if (text == nullptr || conditions_read_only_) {
    return;
  }
  if (conditions_dc_edit_) {
    AppendBoundedNumber(conditions_dc_buffer_, text, 0, kMaxSaveDc, false);
    return;
  }
  if (conditions_custom_edit_) {
    if (conditions_custom_ignore_text_) {
      conditions_custom_ignore_text_ = false;
      return;
    }
    AppendUtf8(conditions_custom_buffer_, text, kMaxConditionNameBytes);
  }
}

auto App::ApplyConditionDc() -> void {
  const auto& standard = DefaultConditions();
  if (!std::cmp_greater_equal(conditions_selected_, standard.size())) {
    const std::string& name = standard[conditions_selected_];
    const auto it = std::ranges::find_if(
        conditions_draft_,
        [&name](const Condition& item) -> bool { return item.name == name; });
    if (it == conditions_draft_.end()) {
      view_.AddLocalLine(std::string(strings::kConditionDcNeedActive));
    } else {
      int dc = 0;
      if (!conditions_dc_buffer_.empty() &&
          !ParseNumber(conditions_dc_buffer_, dc)) {
        dc = 0;
      }
      it->save_dc = std::clamp(dc, 0, kMaxSaveDc);
      SendCommand(UpdateTokenCombat{.id = conditions_token_id_,
                                    .conditions = conditions_draft_});
    }
  }
  conditions_dc_edit_ = false;
  conditions_dc_buffer_.clear();
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
  if (!IsDm()) {
    view_.AddLocalLine(std::string(strings::kColorDmOnly));
    return;
  }
  SendCommand(SetTokenColor{.id = token->id, .color = NextTokenColor(*token)});
}

auto App::CycleEditorTokenColor() -> void {
  Token* token = FindEditorToken(selected_id_);
  if (token == nullptr) {
    editor_status_.Set(std::string(strings::kSelectToken));
    return;
  }
  token->color = NextTokenColor(*token);
  editor_status_.Set(std::string(strings::kEditorTokenColorChanged));
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
      // Подложка — авторская часть сцены: в игре её не меняют.
      view_.AddLocalLine(std::string(strings::kMapEditorOnly));
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
      } else if (command.kind == ChatCommandKind::kSceneNew ||
                 command.kind == ChatCommandKind::kSceneRename) {
        // В игре сцены только загружаются: структуру правит редактор карт.
        view_.AddLocalLine(std::string(strings::kScenesEditorOnly));
      } else {
        SwitchSceneByName(command.text);
      }
      return;
    case ChatCommandKind::kAoe:
      if (options_.mode != AppMode::kHost) {
        view_.AddLocalLine("Урон по области наносит только DM");
        return;
      }
      SendCommand(AoeDamage{.expression = command.text,
                            .save_dc = command.save_dc,
                            .damage_type = command.damage_type});
      return;
    case ChatCommandKind::kOwner:
      if (token == nullptr) {
        view_.AddLocalLine(std::string(strings::kSelectTokenForCommand));
        return;
      }
      SendCommand(SetTokenOwner{.id = token->id, .owner = command.text});
      return;
    case ChatCommandKind::kRename:
      if (!IsDm()) {
        view_.AddLocalLine(std::string(strings::kRenameDmOnly));
        return;
      }
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
        SendCommand(DamageToken{.id = token->id,
                                .amount = command.value,
                                .damage_type = command.damage_type});
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
      SendCommand(
          RestParty{.long_rest = command.long_rest, .hit_dice = command.value});
      return;
    case ChatCommandKind::kFall: {
      if (options_.mode != AppMode::kHost) {
        view_.AddLocalLine(std::string(strings::kFallDmOnly));
        return;
      }
      const Token* token = FindSelectedToken();
      if (token == nullptr) {
        view_.AddLocalLine(std::string(strings::kSelectTokenForCommand));
        return;
      }
      SendCommand(FallToken{.id = token->id, .feet = command.value});
      return;
    }
    case ChatCommandKind::kTime:
      if (options_.mode != AppMode::kHost) {
        view_.AddLocalLine(std::string(strings::kTimeDmOnly));
        return;
      }
      SendCommand(AdvanceTime{.seconds = command.value});
      return;
    case ChatCommandKind::kReveal:
    case ChatCommandKind::kUnreveal:
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
        view_.AddLocalLine("Менять видимость можно только на клетках карты");
        return;
      }
      if (command.kind == ChatCommandKind::kUnreveal) {
        SendCommand(
            HideArea{.cell = reveal_cell, .radius = kRevealRadiusCells});
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
    MarkPartyDirty();
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
  // После изменения мира каждому игроку уходит только его дельта: токены,
  // вошедшие в туман или ушедшие из него, и изменённые клетки тумана.
  // Полный снимок остаётся для входа и смены сцены.
  if (world_changed) {
    for (const PeerInfo& peer : peers) {
      for (const Event& event : server_.TakeViewDelta(peer.name)) {
        net_server_.SendEvent(peer.id, event);
      }
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
    // Полный вид уже отправлен: дальше игрок получает только изменения.
    server_.MarkViewSent(peer.name);
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
    if (!reconnected) {
      // Вход за новый стол: подсказки прошлой игры остаются в журнале F3.
      view_.ClearLog();
    }
    view_.Reset(update.welcome->snapshot, update.welcome->fog);
    PruneStaleViewState();
    client_connected_ = true;
    client_is_dm_ = update.welcome->is_dm;
    client_connecting_ = false;
    in_game_ = true;
    menu_.busy = false;
    menu_.status.clear();
    version_mismatch_.reset();
    CenterCamera();
    if (reconnected) {
      view_.AddLocalLine(std::string(strings::kConnectionRestored));
    }
    SDL_Log("joined host, world snapshot received");
  }
  for (const Event& event : update.events) {
    view_.Apply(event);
  }
  // Как только у игрока появился персонаж, выбор считается сделанным: даже
  // если DM потом снимет владельца, «забрать» больше не предлагаем.
  if (PlayerHasCharacter()) {
    claimed_once_ = true;
  }
  if (update.roster.has_value()) {
    UpdatePlayersFromRoster(*update.roster);
  }
  if (update.mismatch.has_value()) {
    version_mismatch_ = *update.mismatch;
    // Диалог сам объясняет отказ: строка «Подключение...» под ним только
    // путает.
    menu_.status.clear();
    std::string latest = "нет";
    if (update.mismatch->latest_version.has_value()) {
      latest = FormatVersion(*update.mismatch->latest_version);
    }
    SDL_Log("host protocol %u (version %s), latest release %s",
            static_cast<unsigned>(update.mismatch->host_protocol),
            FormatVersion(update.mismatch->host_version).c_str(),
            latest.c_str());
  }
  if (update.reject.has_value()) {
    if (in_game_ && net_client_.Welcomed()) {
      view_.AddLocalLine(std::string(strings::kRejected) + *update.reject);
    } else if (in_game_) {
      // Меню под диалогом версий показывать нечего: текст отказа не нужен.
      if (!version_mismatch_.has_value()) {
        menu_.status = std::string(strings::kRejected) + *update.reject;
      }
      menu_.busy = false;
      client_connecting_ = false;
      in_game_ = false;
      net_client_.Disconnect();
    } else {
      if (!version_mismatch_.has_value()) {
        menu_.status = std::string(strings::kRejected) + *update.reject;
      }
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
  if (actions_active_ && !has_token(actions_token_id_)) {
    CloseActions();
  }
}

auto App::ScrollLog(int lines) -> void {
  log_scroll_.Scroll(lines, view_.Log().size());
}

auto App::SyncLogScroll() -> void { log_scroll_.OnAppend(view_.Log().size()); }

auto App::OpenLogHistory() -> void {
  // Сначала сбрасываем буфер: в файле должны быть строки этого кадра.
  FlushSessionLog();
  history_lines_ = LoadSessionLog(SessionLogPath().string(), kMaxHistoryLines);
  history_query_.clear();
  history_matches_ = SearchSessionLog(history_lines_, history_query_);
  history_scroll_.SetVisibleLines(
      LogHistoryVisibleRows(renderer_, text_.get()));
  history_scroll_.ToNewest();
  history_active_ = true;
  help_active_ = false;
  chat_.Close();
}

auto App::CloseLogHistory() -> void {
  history_active_ = false;
  history_query_.clear();
  history_matches_.clear();
  history_lines_.clear();
}

auto App::HandleLogHistoryTextInput(const char* text) -> void {
  if (text == nullptr) {
    return;
  }
  const std::string before = history_query_;
  AppendUtf8(history_query_, text, 64);
  if (history_query_ == before) {
    return;
  }
  history_matches_ = SearchSessionLog(history_lines_, history_query_);
  history_scroll_.ToNewest();
}

auto App::HandleLogHistoryKeyDown(const SDL_KeyboardEvent& key) -> void {
  const int visible = LogHistoryVisibleRows(renderer_, text_.get());
  history_scroll_.SetVisibleLines(visible);
  switch (key.key) {
    case SDLK_ESCAPE:
      CloseLogHistory();
      break;
    case SDLK_BACKSPACE:
      if (!history_query_.empty()) {
        EraseLastCodePoint(history_query_);
        history_matches_ = SearchSessionLog(history_lines_, history_query_);
        history_scroll_.ToNewest();
      }
      break;
    case SDLK_UP:
      history_scroll_.Scroll(1, history_matches_.size());
      break;
    case SDLK_DOWN:
      history_scroll_.Scroll(-1, history_matches_.size());
      break;
    case SDLK_PAGEUP:
      history_scroll_.Scroll(visible, history_matches_.size());
      break;
    case SDLK_PAGEDOWN:
      history_scroll_.Scroll(-visible, history_matches_.size());
      break;
    case SDLK_HOME:
      history_scroll_.Scroll(static_cast<int>(history_matches_.size()),
                             history_matches_.size());
      break;
    case SDLK_END:
      history_scroll_.ToNewest();
      break;
    default:
      break;
  }
}

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
  if (!in_game_) {
    return;
  }
  StoreActiveScene();
  if (SaveOverlay()) {
    SDL_Log("party saved");
  }
}

auto App::LoadGame() -> void {
  if (options_.mode == AppMode::kClient) {
    SDL_Log("loading is only available on the host");
    return;
  }
  if (!in_game_ || !campaign_file_loaded_) {
    return;
  }
  // F9 читает именно диск: незаписанную просьбу сначала доводим до конца,
  // чтобы не смешать её с тем, что уже лежит в файле.
  overlay_saver_.Flush();
  const std::filesystem::path path = OverlayFilePath();
  std::error_code error;
  if (!std::filesystem::exists(path, error)) {
    view_.AddLocalLine(std::string(strings::kPartyMissing));
    SDL_Log("party load failed: no file");
    return;
  }
  const std::optional<Overlay> loaded = ReadOverlay(path);
  if (!loaded.has_value()) {
    view_.AddLocalLine(std::string(strings::kPartyReadFailed) +
                       path.filename().string());
    SDL_Log("party load failed: unreadable file");
    return;
  }
  if (loaded->campaign_fingerprint != CampaignFingerprint(base_campaign_)) {
    view_.AddLocalLine(std::string(strings::kPartyMismatchInGame));
    SDL_Log("party load failed: campaign fingerprint mismatch");
    return;
  }
  overlay_ = *loaded;
  party_error_ = false;
  party_mismatch_ = false;
  ApplyPartyToCampaign();
  if (campaign_.scenes.empty()) {
    return;
  }
  ResetWorld(campaign_.scenes[campaign_.active].snapshot);
  autosave_.Saved();
  saved_revision_ = world_revision_;
  view_.AddLocalLine(std::string(strings::kPartyLoaded));
  SDL_Log("party loaded");
}

auto App::SaveCampaign() -> void {
  // Кампания — авторский файл: игра пишет только оверлей.
  if (!in_editor_) {
    SDL_Log("campaign save is editor-only");
    return;
  }
  RequestCampaignSave();
  // Явное сохранение доводим до диска: кадр подождёт, зато сообщение честное.
  if (!campaign_saver_.Flush()) {
    SDL_Log("campaign save failed");
    return;
  }
  // Пока шла запись, главный поток был занят, поэтому мир на диске свежий.
  autosave_.Saved();
}

auto App::RequestCampaignSave() -> void {
  const std::string file = campaign_files_.empty()
                               ? std::string("campaign.dndcamp")
                               : campaign_files_[campaign_index_];
  // Редактор пишет авторский шаблон: следы партии в файл не попадают.
  for (Scene& scene : campaign_.scenes) {
    scene.snapshot = CleanTemplateSnapshot(scene.snapshot);
  }
  // Копия кампании снимается сейчас: правки после Request ждут следующего
  // прохода, и ChangedDuringSave это учтёт.
  saved_revision_ = world_revision_;
  campaign_saver_.Request(campaign_, AssetPath(file));
}

auto App::SaveOverlay() -> bool {
  if (!campaign_file_loaded_) {
    return false;
  }
  RequestOverlaySave();
  if (!overlay_saver_.Flush()) {
    overlay_write_failed_ = true;
    SDL_Log("overlay save failed");
    if (in_game_) {
      view_.AddLocalLine(std::string(strings::kPartySaveFailed));
    } else {
      menu_.status = std::string(strings::kPartySaveFailed);
    }
    return false;
  }
  overlay_write_failed_ = false;
  autosave_.Saved();
  party_exists_ = true;
  return true;
}

auto App::RequestOverlaySave() -> void {
  if (!campaign_file_loaded_) {
    return;
  }
  // Копия партии снимается сейчас: игра не ждёт диск.
  saved_revision_ = world_revision_;
  overlay_saver_.Request(overlay_, OverlayFilePath());
}

auto App::OverlayFilePath() const -> std::filesystem::path {
  const std::string file = campaign_files_.empty()
                               ? std::string("campaign.dndcamp")
                               : campaign_files_[campaign_index_];
  return OverlayPath(AssetPath(file));
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

auto App::PrepareDefaultCampaign() -> void {
  // Файла ещё нет: начинаем с отдельной сцены или со стартовой карты.
  const std::optional<GameSnapshot> loaded_scene = ReadSnapshot(ScenePath());
  GameSnapshot snapshot =
      CleanTemplateSnapshot(loaded_scene.value_or(BuildSnapshot()));
  base_campaign_ = MakeCampaign(std::move(snapshot), "Сцена 1");
  campaign_ = base_campaign_;
  overlay_ = FreshOverlay(base_campaign_);
  campaign_file_loaded_ = false;
  party_exists_ = false;
  party_error_ = false;
  party_mismatch_ = false;
  overlay_write_failed_ = false;
  scene_ = campaign_.scenes[campaign_.active].snapshot;
  scene_.encounter = Encounter{};
  server_.Reset(scene_);
  view_.Reset(server_.DmSnapshot(), server_.FullFogState());
}

auto App::ApplyPartyToCampaign() -> void {
  const OverlayMerge merged = ApplyOverlay(base_campaign_, overlay_);
  campaign_ = merged.campaign;
  if (!merged.rolled_back) {
    return;
  }
  // Партия спрятала все сцены: продолжаем кампанию и честно об этом говорим.
  if (in_game_) {
    view_.AddLocalLine(std::string(strings::kPartyRolledBack));
  } else {
    menu_.status = std::string(strings::kPartyRolledBack);
  }
}

auto App::LoadSelectedCampaign(LoadMode mode) -> bool {
  if (campaign_files_.empty()) {
    PrepareDefaultCampaign();
    return false;
  }
  const std::string file = campaign_files_[campaign_index_];
  const std::filesystem::path path = AssetPath(file);
  // Фоновая запись могла ещё не закрыть файл: читаем только после неё.
  campaign_saver_.Flush();
  overlay_saver_.Flush();
  if (mode == LoadMode::kEditor) {
    // Редактор не должен унаследовать слитую с партией копию.
    campaign_ = base_campaign_;
  }
  std::error_code error;
  if (!std::filesystem::exists(path, error)) {
    // Пустой слот: кампания создастся при первом старте хоста или F5.
    PrepareDefaultCampaign();
    return false;
  }
  const std::optional<Campaign> loaded = ReadCampaign(path);
  if (!loaded.has_value()) {
    menu_.status = "Не удалось прочитать " + file;
    return false;
  }
  base_campaign_ = *loaded;
  campaign_ = *loaded;
  campaign_file_loaded_ = true;

  if (mode == LoadMode::kGame) {
    party_exists_ = false;
    party_error_ = false;
    party_mismatch_ = false;
    overlay_ = FreshOverlay(base_campaign_);
    const std::filesystem::path overlay_path = OverlayPath(path);
    if (std::filesystem::exists(overlay_path, error)) {
      party_exists_ = true;
      const std::optional<Overlay> party = ReadOverlay(overlay_path);
      if (!party.has_value()) {
        // Повреждённый файл не затираем и не игнорируем молча.
        party_error_ = true;
        menu_.status = std::string(strings::kPartyReadFailed) +
                       overlay_path.filename().string();
      } else {
        overlay_ = *party;
        if (overlay_.campaign_fingerprint !=
            CampaignFingerprint(base_campaign_)) {
          // Чужая партия ждёт подтверждения DM.
          party_mismatch_ = true;
          menu_.status = std::string(strings::kPartyMismatchBlocked);
        } else {
          ApplyPartyToCampaign();
        }
      }
    }
  }

  if (campaign_.scenes.empty()) {
    PrepareDefaultCampaign();
    return false;
  }
  // Оверлей теперь ровно тот, что на диске (или свежий): старую ошибку
  // записи больше не держим. Редактор оверлей не перечитывает.
  if (mode == LoadMode::kGame) {
    overlay_write_failed_ = false;
  }
  scene_ = campaign_.scenes[campaign_.active].snapshot;
  scene_.encounter = Encounter{};
  server_.Reset(scene_);
  view_.Reset(server_.DmSnapshot(), server_.FullFogState());
  if (mode == LoadMode::kEditor) {
    menu_.status.clear();
    return true;
  }
  if (!party_error_ && !party_mismatch_) {
    menu_.status.clear();
  }
  return !party_error_ && !party_mismatch_;
}

auto App::StartNewParty() -> void {
  overlay_ = FreshOverlay(base_campaign_);
  party_error_ = false;
  party_mismatch_ = false;
  new_party_confirm_.Clear();
  view_.ClearLog();
  ApplyPartyToCampaign();
  if (campaign_.scenes.empty()) {
    return;
  }
  GameSnapshot snapshot = campaign_.scenes[campaign_.active].snapshot;
  snapshot.encounter = Encounter{};
  if (in_game_) {
    ResetWorld(std::move(snapshot));
    view_.AddLocalLine(std::string(strings::kNewPartyStarted));
  } else {
    scene_ = snapshot;
    server_.Reset(scene_);
    view_.Reset(server_.DmSnapshot(), server_.FullFogState());
  }
  const bool saved = SaveOverlay();
  if (saved || !campaign_file_loaded_) {
    menu_.status = std::string(strings::kNewPartyStarted);
  }
}

auto App::AcceptParty() -> void {
  if (!party_mismatch_) {
    return;
  }
  overlay_.campaign_fingerprint = CampaignFingerprint(base_campaign_);
  party_mismatch_ = false;
  party_error_ = false;
  ApplyPartyToCampaign();
  if (campaign_.scenes.empty()) {
    return;
  }
  scene_ = campaign_.scenes[campaign_.active].snapshot;
  scene_.encounter = Encounter{};
  server_.Reset(scene_);
  view_.Reset(server_.DmSnapshot(), server_.FullFogState());
  if (SaveOverlay()) {
    menu_.status = std::string(strings::kPartyAccepted);
  }
}

auto App::ResetActiveSceneToCampaign() -> void {
  if (party_mismatch_ || party_error_) {
    menu_.status = std::string(strings::kPartyNeedsDecision);
    return;
  }
  if (campaign_.scenes.empty() || campaign_.active >= campaign_.scenes.size()) {
    return;
  }
  const SceneId id = campaign_.scenes[campaign_.active].id;
  if (campaign_.scenes[campaign_.active].overlay_only) {
    menu_.status = std::string(strings::kSceneNotInCampaign);
    return;
  }
  if (!ClearScene(overlay_, id)) {
    menu_.status = std::string(strings::kSceneAlreadyTemplate);
    return;
  }
  ApplyPartyToCampaign();
  const std::size_t index = FindSceneIndex(campaign_, id);
  campaign_.active = index != kNoScene ? index : 0;
  SetActive(overlay_, campaign_.scenes[campaign_.active].id);
  GameSnapshot snapshot = campaign_.scenes[campaign_.active].snapshot;
  snapshot.encounter = Encounter{};
  if (in_game_) {
    ResetWorld(std::move(snapshot));
  } else {
    scene_ = snapshot;
    server_.Reset(scene_);
    view_.Reset(server_.DmSnapshot(), server_.FullFogState());
  }
  if (SaveOverlay()) {
    menu_.status = std::string(strings::kSceneReset);
  }
}

auto App::HandleMenuCommand(MenuCommand command) -> void {
  switch (command) {
    case MenuCommand::kNewParty: {
      const auto now = std::chrono::steady_clock::now();
      if (new_party_confirm_.Request(1, now)) {
        StartNewParty();
      } else {
        menu_.status = std::string(strings::kNewPartyConfirm);
      }
      break;
    }
    case MenuCommand::kResetScene:
      ResetActiveSceneToCampaign();
      break;
    case MenuCommand::kAcceptParty:
      AcceptParty();
      break;
    case MenuCommand::kNone:
      break;
  }
}

auto App::SelectCampaign(int delta) -> void {
  if (campaign_files_.empty() || delta == 0) {
    return;
  }
  // Недописанную партию нельзя бросить при смене кампании: сначала доводим
  // запись до диска. Ошибка остаётся в меню и блокирует переход.
  const bool overlay_pending = overlay_saver_.Busy() || overlay_write_failed_;
  if (campaign_file_loaded_ && overlay_pending) {
    RequestOverlaySave();
    if (!overlay_saver_.Flush()) {
      overlay_write_failed_ = true;
      menu_.status = std::string(strings::kPartySaveFailed);
      SDL_Log("campaign switch blocked: overlay save failed");
      return;
    }
    overlay_write_failed_ = false;
  }
  campaign_index_ = WrapIndex(campaign_index_, delta, campaign_files_.size());
  new_party_confirm_.Clear();
  (void)LoadSelectedCampaign(LoadMode::kGame);
}

auto App::MarkPartyDirty() -> void {
  ++world_revision_;
  autosave_.MarkChanged(std::chrono::steady_clock::now());
}

auto App::CheckAutosave() -> void {
  const auto now = std::chrono::steady_clock::now();
  if (const std::optional<bool> result = overlay_saver_.TakeResult()) {
    if (!*result) {
      overlay_write_failed_ = true;
      SDL_Log("overlay save failed");
      // Запись не удалась: не долбим диск каждый кадр, повторим позже.
      autosave_.SaveFailed(now);
    } else if (world_revision_ != saved_revision_) {
      // Мир изменился, пока шла запись: нужен ещё один проход после паузы.
      autosave_.ChangedDuringSave(now);
    } else {
      overlay_write_failed_ = false;
      autosave_.Saved();
      SDL_Log("party autosaved");
    }
  }
  if (!autosave_.Dirty() || options_.mode != AppMode::kHost || !in_game_ ||
      !campaign_file_loaded_ || overlay_saver_.Busy()) {
    return;
  }
  if (!autosave_.Due(now)) {
    return;
  }
  StoreActiveScene();
  RequestOverlaySave();
}

auto App::StoreActiveScene() -> void {
  if (campaign_.scenes.empty() || campaign_.active >= campaign_.scenes.size()) {
    return;
  }
  const GameSnapshot snapshot = server_.Snapshot();
  // scene_ — то, что видит ClientView, кампания — рабочая копия, оверлей —
  // то, что уйдёт на диск.
  scene_ = snapshot;
  Scene& active = campaign_.scenes[campaign_.active];
  active.snapshot = snapshot;
  if (!in_game_) {
    return;
  }
  (void)SetSceneSnapshot(overlay_, active.id, snapshot);
  SetActive(overlay_, active.id);
}

auto App::BroadcastWorld() -> void {
  for (const PeerInfo& peer : net_server_.Peers()) {
    net_server_.SendEvent(
        peer.id, ViewUpdated{.snapshot = server_.ClientSnapshot(peer.name),
                             .fog = server_.FogState(peer.name)});
    server_.MarkViewSent(peer.name);
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
  sheet_read_only_ = false;
  conditions_active_ = false;
  conditions_read_only_ = false;
  bestiary_active_ = false;
  delete_token_.Clear();
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
  MarkPartyDirty();
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
    RequestOverlaySave();
    return;
  }
  campaign_.active = index;
  SetActive(overlay_, campaign_.scenes[index].id);
  const std::string name = campaign_.scenes[index].name;
  ResetWorld(campaign_.scenes[index].snapshot);
  SDL_Log("scene '%s' is active", name.c_str());
  Announce("Сцена: " + name);
  RequestOverlaySave();
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

auto App::OpenScenes() -> void {
  if (options_.mode != AppMode::kHost) {
    view_.AddLocalLine(std::string(strings::kScenesDmOnly));
    return;
  }
  scenes_active_ = true;
  scenes_selected_ = campaign_.active;
  bestiary_active_ = false;
  sheet_active_ = false;
  sheet_read_only_ = false;
  conditions_active_ = false;
  conditions_read_only_ = false;
  actions_active_ = false;
  combat_active_ = false;
  chat_.Close();
}

auto App::CloseScenes() -> void { scenes_active_ = false; }

auto App::HandleScenesMouseDown(const SDL_MouseButtonEvent& button) -> void {
  if (button.button != SDL_BUTTON_LEFT) {
    return;
  }
  const std::size_t index = SceneRowAt(renderer_, text_.get(), campaign_.scenes,
                                       scenes_selected_, button.x, button.y);
  if (index == kNoScene) {
    return;
  }
  scenes_selected_ = index;
  if (button.clicks >= 2) {
    CloseScenes();
    SwitchScene(index);
  }
}

auto App::HandleScenesKeyDown(const SDL_KeyboardEvent& key) -> void {
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

auto App::Iterate() -> void {
  // Статусы панелей гаснут сами: подсказка возвращается через 3 секунды.
  const auto status_now = std::chrono::steady_clock::now();
  editor_status_.Expire(status_now);
  editor_scenes_status_.Expire(status_now);

  if (text_ != nullptr) {
    text_->BeginFrame();
  }

  if (in_editor_) {
    ClearBoard(renderer_);
    DrawBackground(scene_.map, scene_.background);
    DrawGrid(renderer_, scene_.map, camera_.cell_size, camera_.x, camera_.y);
    DrawTerrain(renderer_, scene_.map, camera_.cell_size, camera_.x, camera_.y);
    std::vector<TokenArt> editor_art;
    editor_art.reserve(scene_.tokens.size());
    for (const Token& token : scene_.tokens) {
      editor_art.push_back(
          TokenArt{.portrait = PortraitTexture(token.portrait)});
    }
    DrawTokens(renderer_, scene_.tokens, camera_.cell_size, camera_.x,
               camera_.y, text_.get(), selected_id_, 0, &editor_art);
    if (editor_panel_visible_) {
      const std::string map_name = editor_scene_ < campaign_.scenes.size()
                                       ? campaign_.scenes[editor_scene_].name
                                       : std::string{};
      std::string edit_label;
      std::string edit_buffer;
      if (editor_background_edit_) {
        edit_label = std::string(strings::kEditorBackgroundLabel);
        edit_buffer = editor_background_buffer_;
      } else if (editor_token_edit_) {
        edit_label = std::string(strings::kEditorTokenNameLabel);
        edit_buffer = editor_token_edit_buffer_;
      }
      DrawEditorPanel(renderer_, text_.get(), scene_, editor_brush_,
                      editor_scene_, campaign_.scenes.size(), map_name,
                      editor_status_.Text(), selected_id_, edit_label,
                      edit_buffer);
    }
    if (editor_scenes_active_) {
      const std::string edit_label = editor_scene_edit_
                                         ? std::string(strings::kSceneNameLabel)
                                         : std::string{};
      DrawScenesPanel(renderer_, text_.get(), campaign_.scenes,
                      campaign_.active, editor_scenes_selected_, edit_label,
                      editor_scene_edit_buffer_, editor_scenes_status_.Text(),
                      strings::kEditorScenesHint);
    }
    if (bestiary_active_) {
      DrawBestiaryPanel(renderer_, text_.get(), DefaultBestiary());
    }
    DrawActiveSheet();
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
    const PartySummary party =
        SummarizeParty(overlay_, party_exists_, party_mismatch_);
    menu_.party_state = party.state;
    menu_.party_scenes = party.changed_scenes;
    DrawMenu(renderer_, text_.get(), menu_, local_addresses_, MenuUpdateState(),
             MenuMismatchState());
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

  const bool is_dm = IsDm();
  ClearBoard(renderer_);
  DrawBackground(view_.Map(), view_.Background());
  DrawGrid(renderer_, view_.Map(), camera_.cell_size, camera_.x, camera_.y);
  DrawTerrain(renderer_, view_.Map(), camera_.cell_size, camera_.x, camera_.y);
  const Encounter& encounter = view_.EncounterState();
  if (show_movement_range_) {
    const Token* range_token = FindSelectedToken();
    if (range_token != nullptr) {
      const int budget = EffectiveSpeedCells(*range_token);
      int radius = budget;
      if (encounter.active && !is_dm) {
        radius = RemainingMovementCells(*range_token);
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
  if (encounter.active) {
    DrawInitiativePanel(renderer_, text_.get(), view_.Tokens(), encounter);
  }
  const bool is_client = options_.mode == AppMode::kClient;
  const Token* selected = FindSelectedToken();
  const bool can_claim = is_client && selected != nullptr &&
                         selected->kind == TokenKind::kPlayer &&
                         selected->owner.empty() && !PlayerHasCharacter() &&
                         !claimed_once_;
  MovementInfo movement = MovementInfo::kFree;
  if (is_dm) {
    movement = MovementInfo::kHidden;
  } else if (encounter.active) {
    movement = MovementInfo::kBudget;
  }
  const float token_bottom =
      DrawTokenPanel(renderer_, text_.get(), selected, options_.player_name,
                     is_client, can_claim, movement, encounter.active);
  const float players_y = token_bottom > 0.0F ? token_bottom + 12.0F : 280.0F;
  players_top_ = players_y;
  DrawPlayersPanel(renderer_, text_.get(), players_, players_y);
  if (dragging_ && drag_target_.has_value()) {
    DrawCellHighlight(renderer_, *drag_target_, camera_.cell_size, camera_.x,
                      camera_.y);
  }
  log_top_ = DrawLogPanel(renderer_, text_.get(), view_.Log(), chat_.Active(),
                          std::string(chat_.Text()), BottomHintText(),
                          log_scroll_.Offset());
  DrawActiveSheet();
  if (bestiary_active_) {
    DrawBestiaryPanel(renderer_, text_.get(), DefaultBestiary());
  }
  if (scenes_active_) {
    DrawScenesPanel(renderer_, text_.get(), campaign_.scenes, campaign_.active,
                    scenes_selected_, {}, {}, {}, strings::kScenesHint);
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
                          conditions_draft_, conditions_read_only_,
                          conditions_selected_, conditions_dc_edit_,
                          conditions_dc_buffer_, conditions_custom_edit_,
                          conditions_custom_buffer_);
    }
  }
  if (actions_active_) {
    const Token* action_token = nullptr;
    for (const Token& token : view_.Tokens()) {
      if (token.id == actions_token_id_) {
        action_token = &token;
        break;
      }
    }
    if (action_token == nullptr) {
      CloseActions();
    } else {
      const bool own_turn =
          !encounter.active || is_dm ||
          (encounter.turn_index < encounter.order.size() &&
           encounter.order[encounter.turn_index] == action_token->id);
      DrawActionsPanel(renderer_, text_.get(), *action_token, own_turn);
    }
  }
  if (combat_active_) {
    if (!encounter.active || view_.Tokens().empty()) {
      CloseCombat();
    } else {
      if (combat_selected_ >= view_.Tokens().size()) {
        combat_selected_ = view_.Tokens().size() - 1;
      }
      DrawCombatPanel(renderer_, text_.get(), view_.Tokens(), encounter,
                      combat_selected_);
    }
  }
  if (history_active_) {
    // Окно могли увеличить или уменьшить, пока журнал открыт: страница
    // прокрутки должна совпадать с тем, сколько строк рисуется.
    history_scroll_.SetVisibleLines(
        LogHistoryVisibleRows(renderer_, text_.get()));
    history_scroll_.Scroll(0, history_matches_.size());
    DrawLogHistoryPanel(renderer_, text_.get(), history_lines_,
                        history_matches_, history_query_,
                        history_scroll_.Offset());
  }
  // Клиент остаётся на последнем снапшоте, пока идёт переподключение; баннер
  // нужен, чтобы он не принял замороженный стол за настоящий.
  if (is_client && !client_connected_) {
    DrawConnectionBanner(renderer_, text_.get(), strings::kConnectionLost);
  } else {
    // Подмена exe возможна только из меню: во время партии DM видит, что
    // обновление ждёт выхода, и стол не теряется.
    const std::string update = SessionUpdateText();
    if (!update.empty()) {
      DrawConnectionBanner(renderer_, text_.get(), update);
    }
  }
  // Справка рисуется последней и перекрывает все остальные панели.
  if (help_active_) {
    DrawHelpPanel(renderer_, text_.get(), is_dm);
  }
  if (exit_confirm_active_) {
    DrawExitToMenuPanel(renderer_, text_.get(),
                        options_.mode == AppMode::kHost);
  }
  SDL_RenderPresent(renderer_);
}

}  // namespace dnd
