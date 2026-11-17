# AGENTS.md

C++20 + SDL3 virtual tabletop. One executable (`dnd`) acts as either the
authoritative host/DM or a TCP client. No README or CI; the build scripts and
tests are the source of truth.

## Build and test

- Windows: `.\build.bat` (default debug). Profiles: `-p debug|release|relwithdebinfo|minsize`; `-c` forces reconfigure; `-j N` sets jobs.
- `build.bat` wraps `build.sh` in MSYS2 UCRT64. It expects MSYS2 at `C:\msys64` or `%MSYS2_ROOT%`; do not run `build.sh` directly from PowerShell.
- Binaries land in `.build/<profile>/` (e.g. `.build/debug/dnd.exe`).
- First configure downloads SDL3, SDL3_ttf, SDL3_image, and SDL3_net via CMake FetchContent (network required, several minutes). Later runs reuse `.build/<profile>` unless `-c` is passed.
- Run all tests: `ctest --test-dir .build/debug --output-on-failure`
- One suite: `ctest --test-dir .build/debug -R core_tests` or run `.build/debug/dnd_core_tests.exe` directly.
- Tests are plain `main()` binaries with an `Expect` helper, not a test framework. There is no per-test-case CLI filter; to isolate a case, add a `TestX()` and call it from `main()` in `tests/core_tests.cpp` or `tests/net_tests.cpp`.
- `net_tests` binds an ephemeral localhost TCP port and polls with 3-second deadlines; no external services are needed.
- The GUI executable needs a display; debug it with `.vscode/launch.json`, not from tests.

## Architecture

- `src/core`: SDL-free game logic. `GameState` holds rules, `GameServer` validates commands, ownership, fog, and dice, `ClientView` is the only state rendering may read, and `LocalTransport` is the in-process command/event queue.
- `src/net`: TCP framing, sockets, and client/server transports. `NetServer` is not authoritative; `App` owns the `GameServer` and feeds it peer commands.
- `src/app`: SDL window/input, the Host/Join menu, and the host/client pump. `src/render`: drawing only. Entrypoint is `src/main.cpp` using SDL3 main callbacks (`SDL_MAIN_USE_CALLBACKS`).
- Text goes through `render/text_renderer` (`TTF_Text` + `TTF_DrawRendererText`, pooled objects). Call `BeginFrame()` once per rendered frame; do not reintroduce per-frame `TTF_RenderText_Blended` + texture creation.
- Plain launch shows the Host/Join/Map menu. `--host`, `--port`, or `--connect HOST[:PORT]` auto-start that mode; `--editor` opens the map editor; `--menu` forces the menu. Clients also accept `--name` and both sides accept `--password`.
- Host mode is always the DM and also plays locally. The default port is 7777. Player names must be non-empty and unique; the host can kick with `/kick NAME`.
- The map editor saves `scene.dndscene` beside the executable and reuses the snapshot format. Editor keys: `1`–`5` brushes, `[`/`]` width, `-`/`=` height, `F5`/`F9` scene, `Enter` host, `Esc` menu. Middle-drag pans and the wheel zooms in both editor and game.
- Visibility and permissions are enforced in `GameServer` (`ClientSnapshot`, `EventVisibleToPlayers`, `can_control_all`), not in the renderer. Players cannot add/remove tokens or control encounters.
- Party fog is computed from every `TokenKind::kPlayer` token; NPC and monster tokens never grant vision. Unclaimed player tokens are locked until claimed; free NPC tokens with an empty owner may be moved by players. The DM can reassign or release owners.
- F5 saves, F9 loads. The file is `savegame.bin` beside the executable via `SDL_GetBasePath()`, and saving/loading is host-only.

## Serialization and protocol

- `src/core/serialization.cpp` is a hand-written little-endian binary format shared by save files and TCP messages. Decoders are intentionally strict: reject truncation, trailing bytes, and out-of-range values.
- Changing any serialized `Token`, `GameSnapshot`, `Command`, or `Event` field requires updating both encode and decode paths, extending `core_tests`/`net_tests`, and bumping `kSaveVersion` in `src/core/scene_io.h` and `kProtocolVersion` in `src/net/protocol.h`. Version mismatches reject saves and clients.
- `Hello` carries the player name and password. The server enforces the password and broadcasts a `Roster` message after joins, drops, and kicks.
- Save files are `DNDS` magic + version + payload length + encoded snapshot.

## Style

- Google C++ style via `.clang-format` (80 columns, 2-space indent); `auto Fn() -> T` trailing return types are used throughout.
- `.clang-tidy` enforces naming: types/functions CamelCase, locals/variables `lower_case`, private members trailing `_`, constants `kCamelCase`.
- Project headers use paths rooted at `src/`, e.g. `#include "core/game_state.h"`.
- Player-facing UI strings are Russian and live in `src/app/strings.h`. Use `core/utf8.h` for backspace/truncation instead of byte-wise `pop_back`/`resize`.
- Keep `dnd_core` free of SDL dependencies; SDL belongs in `src/app` and `src/render`.
