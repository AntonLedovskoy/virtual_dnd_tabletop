#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <charconv>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>

#include "app/app.h"

namespace {

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

auto ParseOptions(int argc, char** argv) -> dnd::AppOptions {
  dnd::AppOptions options;
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
      SDL_Log(
          "usage: dnd [--menu] [--host] [--editor] [--port N] [--name NAME] "
          "[--password PASS] [--connect HOST[:PORT]]");
    }
  }
  if (force_menu) {
    options.auto_start = false;
  }
  return options;
}

}  // namespace

auto SDL_AppInit(void** appstate, int argc, char* argv[]) -> SDL_AppResult {
  SDL_SetAppMetadata("dnd", "0.1.0", "com.example.dnd");

  auto app = std::make_unique<dnd::App>(ParseOptions(argc, argv));
  if (!app->Initialize()) {
    SDL_Log("app initialization failed: %s", SDL_GetError());
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
}
