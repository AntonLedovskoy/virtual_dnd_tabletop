#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <charconv>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>

#include "app/app.h"
#include "app/file_log.h"
#include "app/instance_lock.h"
#include "app/strings.h"
#include "core/campaign_io.h"
#include "core/scene_io.h"
#include "core/update.h"
#include "core/version.h"
#include "net/protocol.h"

// CMake подставляет версию из git describe; вне CMake-сборки — developer build.
#ifndef DND_VERSION
#define DND_VERSION "0.0.0-dirty"
#endif

namespace {

// Окно консоли у GUI-сборки нет, поэтому справка показывается диалогом.
constexpr const char* kUsage =
    "usage: virtual_dnd_tabletop [--menu] [--host] [--editor] [--port N] "
    "[--name NAME] [--password PASS] [--connect HOST[:PORT]] [--version] "
    "[--no-update-check]\n\n"
    "--no-update-check отключает проверку обновлений. "
    "DND_UPDATE_MANIFEST=путь — локальный latest.json для отладки.";

// `--version` — тоже диалогом: консоли нет. В developer build видно, почему
// автообновление выключено.
auto VersionInfo() -> std::string {
  std::string text = "virtual_dnd_tabletop ";
  dnd::Version version;
  if (dnd::ParseVersion(DND_VERSION, version)) {
    text += dnd::FormatVersion(version);
    if (version.dirty) {
      text += " (developer build)";
    }
  } else {
    text += DND_VERSION;
  }
  text += "\n\nПротокол: " + std::to_string(dnd::kProtocolVersion);
  text += "\nСохранения: " + std::to_string(dnd::kSaveVersion);
  text += "\nКампании: " + std::to_string(dnd::kCampaignVersion);
  return text;
}

// Папка рядом с exe: в ней лежат данные стола и файлы подмены. SDL держит
// строку сама (в SDL3 это const char*), освобождать её нельзя.
auto BasePath() -> std::filesystem::path {
  const char* text = SDL_GetBasePath();
  if (text == nullptr) {
    return {};
  }
  return {
      std::u8string(reinterpret_cast<const char8_t*>(text), std::strlen(text))};
}

auto ParsePort(std::string_view text, std::uint16_t& port) -> bool {
  if (text.empty()) {
    return false;
  }
  unsigned int value = 0;
  const auto result =
      std::from_chars(text.data(), text.data() + text.size(), value);
  if (result.ec != std::errc{} || result.ptr != text.data() + text.size() ||
      value == 0 || value > 65535) {
    return false;
  }
  port = static_cast<std::uint16_t>(value);
  return true;
}

auto ParseEndpoint(std::string_view text, dnd::AppOptions& options) -> bool {
  const auto colon = text.rfind(':');
  if (colon == std::string_view::npos) {
    options.connect_host = std::string(text);
    return !options.connect_host.empty();
  }
  options.connect_host = std::string(text.substr(0, colon));
  return ParsePort(text.substr(colon + 1), options.port) &&
         !options.connect_host.empty();
}

struct CommandLine {
  dnd::AppOptions options;
  // Справка и версия в GUI-сборке показываются диалогом, а не в консоли.
  bool show_help = false;
  bool show_version = false;
};

auto ParseOptions(int argc, char** argv) -> CommandLine {
  CommandLine command_line;
  dnd::AppOptions& options = command_line.options;
  bool force_menu = false;
  for (int i = 1; i < argc; ++i) {
    const std::string_view argument = argv[i];
    if (argument == "--connect" && i + 1 < argc) {
      options.mode = dnd::AppMode::kClient;
      if (ParseEndpoint(argv[++i], options)) {
        options.auto_start = true;
      } else {
        SDL_Log("invalid --connect endpoint '%s'", argv[i]);
        options.mode = dnd::AppMode::kHost;
        options.auto_start = false;
      }
    } else if (argument == "--host") {
      options.mode = dnd::AppMode::kHost;
      options.auto_start = true;
    } else if (argument == "--port" && i + 1 < argc) {
      if (ParsePort(argv[++i], options.port)) {
        options.auto_start = true;
      } else {
        SDL_Log("invalid --port '%s'", argv[i]);
      }
    } else if (argument == "--name" && i + 1 < argc) {
      options.player_name = argv[++i];
    } else if (argument == "--password" && i + 1 < argc) {
      options.password = argv[++i];
    } else if (argument == "--menu") {
      force_menu = true;
    } else if (argument == "--editor") {
      options.start_editor = true;
      options.auto_start = false;
    } else if (argument == "--help" || argument == "-h") {
      command_line.show_help = true;
    } else if (argument == "--version") {
      command_line.show_version = true;
    } else if (argument == "--no-update-check") {
      options.no_update_check = true;
    }
  }
  if (force_menu) {
    options.auto_start = false;
  }
  return command_line;
}

}  // namespace

auto SDL_AppInit(void** appstate, int argc, char* argv[]) -> SDL_AppResult {
  SDL_SetAppMetadata("virtual_dnd_tabletop", DND_VERSION,
                     "com.example.virtual_dnd_tabletop");
  // Окна консоли нет: журнал приложения и сообщения SDL пишутся в
  // virtual_dnd_tabletop.log.
  dnd::InstallFileLog();

  const CommandLine command_line = ParseOptions(argc, argv);
  SDL_Log("virtual_dnd_tabletop %s", DND_VERSION);
  if (command_line.show_version) {
    const std::string version = VersionInfo();
    SDL_Log("%s", version.c_str());
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, "virtual_dnd_tabletop",
                             version.c_str(), nullptr);
    dnd::ShutdownFileLog();
    return SDL_APP_SUCCESS;
  }
  if (command_line.show_help) {
    SDL_Log("%s", kUsage);
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, "virtual_dnd_tabletop",
                             kUsage, nullptr);
    dnd::ShutdownFileLog();
    return SDL_APP_SUCCESS;
  }

  // Один экземпляр на папку: иначе два процесса из одной папки ломают
  // подмену exe, а `old.exe` остаётся залоченным.
  const std::filesystem::path base = BasePath();
  if (!base.empty()) {
    if (!dnd::AcquireInstanceLock(base)) {
      SDL_Log("another instance already runs from %s", base.string().c_str());
      SDL_ShowSimpleMessageBox(
          SDL_MESSAGEBOX_WARNING, "virtual_dnd_tabletop",
          std::string(dnd::strings::kAlreadyRunning).c_str(), nullptr);
      dnd::ShutdownFileLog();
      return SDL_APP_SUCCESS;
    }

    // Обрыв между шагами подмены лечится до создания окна.
    std::error_code code;
    const bool have_exe =
        std::filesystem::exists(base / std::string(dnd::kExecutableName), code);
    const bool have_old = std::filesystem::exists(
        base / std::string(dnd::kPreviousExeName), code);
    const bool have_update =
        std::filesystem::exists(base / std::string(dnd::kUpdateExeName), code);
    const dnd::StartupAction action =
        dnd::PlanStartupRecovery(have_exe, have_old, have_update);
    std::string recovery_error;
    if (!dnd::ApplyStartupRecovery(base, action, recovery_error)) {
      SDL_Log("startup recovery failed: %s", recovery_error.c_str());
    } else if (action != dnd::StartupAction::kNone) {
      SDL_Log("startup recovery: %d", static_cast<int>(action));
    }
  }

  auto app = std::make_unique<dnd::App>(command_line.options);
  if (!app->Initialize()) {
    const std::string error =
        std::string("Не удалось запустить virtual_dnd_tabletop: ") +
        SDL_GetError() + "\n\nПодробности: " + dnd::kFileLogName;
    SDL_Log("app initialization failed: %s", SDL_GetError());
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "virtual_dnd_tabletop",
                             error.c_str(), nullptr);
    dnd::ShutdownFileLog();
    return SDL_APP_FAILURE;
  }

  *appstate = app.release();
  return SDL_APP_CONTINUE;
}

auto SDL_AppEvent(void* appstate, SDL_Event* event) -> SDL_AppResult {
  if (event->type == SDL_EVENT_QUIT) {
    return SDL_APP_SUCCESS;
  }
  static_cast<dnd::App*>(appstate)->HandleEvent(*event);
  return SDL_APP_CONTINUE;
}

auto SDL_AppIterate(void* appstate) -> SDL_AppResult {
  static_cast<dnd::App*>(appstate)->Iterate();
  return SDL_APP_CONTINUE;
}

auto SDL_AppQuit(void* appstate, SDL_AppResult /*result*/) -> void {
  delete static_cast<dnd::App*>(appstate);
  dnd::ShutdownFileLog();
}
