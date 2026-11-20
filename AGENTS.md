# AGENTS.md

Virtual D&D tabletop, C++20 + SDL3. One `dnd` binary: authoritative host/DM or
TCP client. No README/CI: build scripts and tests are the source of truth;
roadmap and weak spots in `PLAN.md`.

Language: code, comments, docs in Russian; identifiers and terms in English.
Local rules win where stricter than global (language, live UI checks).

## Build and tests

- Windows: `.\build.bat` (debug default). `-p debug|release|relwithdebinfo|minsize`, `-c` reconfigure, `-j N` jobs, `-t`/`--tidy` runs `clang-tidy` on our `.cpp`, fails on any finding.
- `build.bat` runs `build.sh` in MSYS2 UCRT64 (`C:\msys64` or `%MSYS2_ROOT%`); never run `build.sh` from PowerShell.
- Binaries in `.build/<profile>/`, e.g. `.build/debug/dnd.exe`.
- First configure fetches SDL3, SDL3_ttf, SDL3_image via FetchContent (network, minutes); `.build/<profile>` reused unless `-c`.
- Our targets: `-Wall -Wextra -Wpedantic`; deps use own flags; new warnings in our code = error.
- All tests: `ctest --test-dir .build/debug --output-on-failure`; one suite: `-R core_tests` or `.build/debug/dnd_core_tests.exe`.
- Tests: plain `main()` + `Expect`, no framework or per-test filter — add `TestX()` and call it from `main()` in `tests/core_tests.cpp`/`tests/net_tests.cpp`.
- `net_tests`: temp localhost port, 3 s timeouts, no external services.
- GUI needs a display: debug via `.vscode/launch.json`, not tests.
- UI: verify on live GUI with client-area screenshots, not `ctest`. Run host + client, walk the scenario, snapshot each changed visual behavior. SendInput/`SendKeys` in client coordinates is fine, outcome must be a snapshot.

## Architecture

- `src/core` — logic without SDL: `GameState` (rules), `GameServer` (validates commands, ownership, fog, dice), `ClientView` (only state rendering reads), `LocalTransport` (in-process command/event queue); login crypto in `src/core/sha256` (SHA-256/HMAC), `src/core/password`.
- `src/net` — TCP framing, sockets, client/server; `NetServer` not authoritative, `App` owns `GameServer`, forwards peer commands. `src/app` — SDL window/input, Host/Join menu, host/client loop; `src/render` — drawing only; entry `src/main.cpp` on SDL3 callbacks (`SDL_MAIN_USE_CALLBACKS`).
- Text only via `render/text_renderer` (`TTF_Text` + `TTF_DrawRendererText`, pooled objects, `BeginFrame()` per frame); per-frame `TTF_RenderText_Blended` textures forbidden.
- Startup: Host/Join/Map menu; `--host`, `--port`, `--connect HOST[:PORT]` start a mode, `--editor` map editor, `--menu` menu. Clients take `--name`, both sides `--password`. Host is always DM, plays locally, port 7777; names non-empty, unique, `/kick NAME` removes a player.
- Editor saves `scene.dndscene` next to `.exe` (snapshot format). Keys: `1`–`5` brushes, `[`/`]` width, `-`/`=` height, `Tab` scenes, `F2` panel, `F5` save scene, `F9` load `scene.dndscene`, `Enter` host, `Esc` menu; scene panel `↑`/`↓`/`1`–`9` select, `Enter` open, `N` new scene from map. Pan middle-drag, zoom wheel, editor and game.
- `L` token sheet (AC, speed, attack/save bonuses, damage, notes), `B` DM bestiary (`src/core/bestiary`, templates copied into token on spawn; 9 templates, `Shift`+digit spawns a group of 3–5 in free cells, `MonsterGroupCells`). Claim = one character per player, `R` releases; DM assigns several via `/owner`. Hints depend on role.
- Connection loss never resets the table: banner "Connection lost, reconnecting…", `NetClient::Reconnect` every 2 s (`kReconnectDelay`). New `Welcome` resends the world; `PruneStaleViewState` clears selection, closes panels of vanished tokens. Dropped player keeps their character; `/owner` with empty name clears ownership; `/kick NAME` bans until host restart.
- Log: `PageUp`/`PageDown`/wheel over bottom panel scroll, `End` to newest, default sticks to bottom. `F1` modal key help (`DrawHelpPanel`, text in `src/app/strings.h`), `Esc`/`F1` closes. `session.log` next to `.exe` (`src/core/session_log.h`) stores `[time] author: text`, survives runs, new session starts with header; `App::FlushSessionLog` writes only new lines via `ClientView::AppendedLogLines`.
- List mouse: initiative row selects token, player row their first character, scene row selects, double click opens scene (editor too). Row geometry shared with rendering: `InitiativeTokenAt`, `PlayerRowAt`, `SceneRowAt` in `src/render/ui_renderer`.
- `Ctrl+=`/`Ctrl+-` UI scale 100/125/150/200%, `Ctrl+0` 100%: logical resolution via `SDL_SetRenderLogicalPresentation`, layout from `RenderSize()`, mouse via `SDL_ConvertEventToRenderCoordinates`.
- `Alt`+RMB or `/ping` drops a "here" marker: `PingAt` → `PingMarker` event for all, 4 s (`kPingSeconds`); in `ClientView::Pings`, pruned by `PrunePings`, never saved.
- Map backdrop: `MapBackground{image, opacity, scale}` in `GameSnapshot`, only the file name travels over the network and the file lives next to each `.exe`; missing file leaves the grid plus a label. DM `/map FILE`, `/map off`, `/map opacity N`, `/map scale N` (`SetMapBackground` → `MapBackgroundUpdated`); render order `ClearBoard` → `DrawMapBackground` → `DrawGrid`, image clipped to the map.
- Token portraits: `Token::portrait` is a file name next to `.exe` (validated by `IsSafeImageName` in `src/core/image_name.h`); `/portrait FILE` by owner or DM, `/portrait off` clears. Tokens are circles built from a `SDL_RenderGeometry` fan (`DrawCircle`/`DrawTexturedCircle` in `src/render/grid_renderer.cpp`); no portrait → token colour plus name.
- Dice: `/roll d20+5`, `2d6+3`, `adv d20+5`, `dis d20-1`; `A` attack (Shift+A advantage), `S` save, `I` initiative. Parser/rolls in `src/core/dice`.
- `K` conditions, `M` movement reach, `Alt`+LMB ruler. Temp HP, conditions in token; reach via `ReachableCells` (`src/core/grid_map`); meters, cell 1.5 m (`src/core/units.h`).
- Damage/heal: `DamageToken`/`HealToken` (`/damage N`, `/heal N` on selected token): temp HP first, 0 HP → "unconscious" (`kUnconscious`), healing clears it. Rest DM-only: `/rest` half max HP, `/longrest` full HP, clears temp HP and conditions; both log (`GameState::DamageToken`, `HealToken`, `RestParty`).
- `Condition{name, rounds}`: `rounds > 0` ticks at own turn start, gone at zero; `0` until removed. `K` panel: standard conditions 10 rounds (`kDefaultConditionRounds`), shows remainder; sheet gives initiative bonus, `StartEncounter` rolls d20 + bonus per token, logs.
- Concentration on damage: d20 + `save_bonus` vs 10 or half damage; failure drops "concentration" (`kConcentration`), logs.
- Death saves: `/deathsave` at 0 HP rolls d20 (10+ success, 1–9 failure, 20 → 1 HP); 3 failures = "dead" (`kDead`), 3 successes remove "unconscious", stabilize. `death_save_successes`/`death_save_failures` in `Token`; downed hit adds a failure, healing/rest reset.
- Combat: player capped by speed per turn — server computes `PathCostCells`, deducts `Token::movement_used`. `M` shows remainder, panel shows "Movement left"; reset at own turn start, combat start/end. DM (`can_control_all`) moves freely, spends nothing; no cap outside combat.
- `F` area template: `1`–`3` circle/cone/line, `←`/`→` rotation, `↑`/`↓` radius/length, `,`/`.` line width, cursor origin; `Shift+F`/`Esc` remove template, `F` only leaves mode. Template (`AoeTemplate`) in `GameSnapshot`, DM-only, visible to all. `Enter` primes `/aoe `; `/aoe 2d6 15` (damage, DC) rolls damage once, each token under template saves d20 + `save_bonus`, success halves. `AoeCells` for cells, damage via `GameState::DamageToken`, logged.
- Visibility/permissions in `GameServer` (`ClientSnapshot`, `EventVisibleToPlayers`, `can_control_all`), not renderer; players cannot add/remove tokens or control combat.
- Party fog from all `TokenKind::kPlayer` tokens; NPCs/monsters grant no vision. Unclaimed characters locked; free NPCs with empty owner movable by players; DM assigns/clears owners.
- `F5` save, `F9` load `savegame.bin` next to `.exe` (`SDL_GetBasePath()`); host only.
- Campaign = scenes (`Campaign`/`Scene` in `src/core/campaign.h`) in `.dndcamp` files next to `.exe` (`src/core/campaign_io.h`, magic `DNDC`, library via `ListCampaignFiles`); the host tab selects the file with `↑`/`↓` or a click, host start and autosave use that file. Scene = full `GameSnapshot` + name + note; active scene mirrors host world (switch, `F5`/`F9`, host start, editor save).
- Scenes DM-only: `Tab`/`/scene` list, `1`–`9` or `↑`/`↓`+`Enter` switch, `R` rename, `E` note, `N` copy current, `Del` twice deletes; `/scene NAME`, `/scene new`, `/scene rename OLD NEW` (spaces ok, `ParseSceneRename`). Switch resets combat, clients get `ViewUpdated`.
- `Scene::name`/`note` — note under name in panel. Duplicate names rejected (`IsSceneNameAvailable`, `RenameScene`), `NextSceneName` picks free; `kCampaignVersion = 4`.
- Host autosave: world-changing command (movement, combat, sheets) sets flag, after 8 s (`kAutosaveDelay`) active scene writes to the selected campaign file; chat/rolls don't set it; `SaveCampaign` clears flag only on successful write. Host menu shows campaign file, active scene and scene count.
- Token/scene delete needs second `Del` within 3 s (`kDeleteConfirmDelay`), first warns. `Ctrl+Z` host: undo last DM action, ring buffer of 10 `GameSnapshot` (`undo_stack_`), snapshot taken before the world-changing command, combat restored too; scene switch/load/host start clear it; campaign structure (create/delete scenes) not undoable.

## Serialization and protocol

- `src/core/serialization.cpp` — hand-written little-endian format for saves and TCP; strict decoders reject truncated data, extra bytes, out-of-range values.
- Serialized field change in `Token`, `GameSnapshot`, `Command`, `Event` → update encode/decode, extend `core_tests`/`net_tests`, bump `kSaveVersion` (`src/core/scene_io.h`) and `kProtocolVersion` (`src/net/protocol.h`); mismatch rejects saves and clients.
- Login: `Hello` = player name + password fingerprint (password never sent). Host sends `Challenge` with 16-byte nonce, client answers `Hello` with HMAC-SHA256(password, nonce) (`src/core/password`, `src/core/sha256`); server checks fingerprint, broadcasts `Roster` on join/leave/kick. Protocol version in `Hello` header; mismatch → `Reject` with both versions, so first five bytes (`DecodeHelloVersion`) layout is frozen. Login attempts share the message rate limit; brute force closes the connection.
- Noisy peers: `kMaxPeers` (8) connections (else "no free slots on the host"), `kMaxCommandsPerSecond` (30) commands/s per player (excess silently dropped), send queue `kMaxOutgoingBytes` (1 MiB) — overflow drops the connection, not the host; `TCP_NODELAY` on.
- Save: magic `DNDS`, version, payload length, snapshot. Campaign: magic `DNDC`, `kCampaignVersion` (`src/core/campaign_io.h`), payload length, scene count, active scene, scenes (name + nested snapshot); campaign version bumped separately from `kSaveVersion`.

## Style

- Google C++ via `.clang-format` (80 columns, 2-space indent), trailing return types `auto Fn() -> T`.
- `.clang-tidy` names: types/functions CamelCase, locals/variables `lower_case`, private members trailing `_`, constants `kCamelCase`.
- Project headers from `src/`, e.g. `#include "core/game_state.h"`.
- Player strings Russian, in `src/app/strings.h`; use `core/utf8.h` for backspace/truncation, not byte-wise `pop_back`/`resize`.
- `dnd_core` must not depend on SDL; SDL only in `src/app` and `src/render`.
