#pragma once

#include <SDL3/SDL.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/client_view.h"
#include "core/game_server.h"
#include "core/local_transport.h"
#include "net/net_client.h"
#include "net/net_server.h"
#include "render/editor_renderer.h"
#include "render/menu_renderer.h"

struct TTF_Font;

namespace dnd {

class TextRenderer;

enum class AppMode : std::uint8_t { kHost, kClient };

struct AppOptions {
  AppMode mode = AppMode::kHost;
  std::string connect_host = "127.0.0.1";
  std::uint16_t port = 7777;
  std::string player_name = "player";
  std::string password;
  bool auto_start = false;
  bool start_editor = false;
};

// Owns the SDL window/renderer and wires input to commands, a local
// authoritative GameServer and a ClientView. Host mode also serves remote
// clients over TCP; client mode talks to a remote host instead of running a
// local server.
class App {
 public:
  explicit App(AppOptions options);
  ~App();

  App(const App&) = delete;
  auto operator=(const App&) -> App& = delete;

  auto Initialize() -> bool;
  auto Shutdown() -> void;

  auto HandleEvent(const SDL_Event& event) -> void;
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
  auto HandlePanMotion(const SDL_MouseMotionEvent& motion) -> void;
  auto HandleWheel(const SDL_MouseWheelEvent& wheel) -> void;
  auto SubmitMenu() -> void;
  auto HandleMenuKeyDown(const SDL_KeyboardEvent& key) -> void;
  auto HandleMenuTextInput(const char* text) -> void;
  auto HandleMenuMouseDown(const SDL_MouseButtonEvent& button) -> void;
  auto FocusNextMenuField(int direction) -> void;
  auto MenuTextBuffer() -> std::string*;
  auto UpdatePlayersFromRoster(const std::vector<RosterEntry>& roster) -> void;
  auto PumpHost() -> void;
  auto PumpClient() -> void;
  auto ApplyOutcome(PeerId from, const CommandOutcome& outcome) -> void;
  auto HandleMouseDown(const SDL_MouseButtonEvent& button) -> void;
  auto HandleMouseUp(const SDL_MouseButtonEvent& button) -> void;
  auto HandleMouseMotion(const SDL_MouseMotionEvent& motion) -> void;
  auto HandleKeyDown(const SDL_KeyboardEvent& key) -> void;
  auto HandleDmHotkey(SDL_Keycode key) -> void;
  auto SubmitMove(Cell target) -> void;
  auto SendCommand(const Command& command) -> void;
  auto SubmitChat() -> void;
  auto ClaimSelectedToken() -> void;
  auto CycleSelectedColor() -> void;
  auto AdjustSelectedHp(int delta) -> void;
  auto AdjustSelectedInitiative(int delta) -> void;
  auto SaveGame() -> void;
  auto LoadGame() -> void;
  auto LoadFont() -> void;

  [[nodiscard]] auto CellFromPixel(float x, float y) const -> Cell;
  [[nodiscard]] auto TokenAt(Cell cell) const -> TokenId;
  [[nodiscard]] auto FindSelectedToken() const -> const Token*;

  AppOptions options_;

  SDL_Window* window_ = nullptr;
  SDL_Renderer* renderer_ = nullptr;
  TTF_Font* font_ = nullptr;
  std::unique_ptr<TextRenderer> text_;
  bool ttf_initialized_ = false;

  GameSnapshot scene_;
  GameServer server_;
  ClientView view_;
  LocalTransport transport_;
  NetServer net_server_;
  NetClient net_client_;

  MenuState menu_;
  std::vector<std::string> local_addresses_;
  std::vector<std::string> players_;
  bool in_game_ = false;
  bool in_editor_ = false;
  bool client_connecting_ = false;

  EditorBrush editor_brush_ = EditorBrush::kWall;
  std::string editor_status_;
  bool editor_panel_visible_ = true;
  float cell_size_ = 48.0F;
  float camera_x_ = 0.0F;
  float camera_y_ = 0.0F;
  bool panning_ = false;
  float pan_last_x_ = 0.0F;
  float pan_last_y_ = 0.0F;

  bool client_connected_ = false;
  bool client_is_dm_ = false;
  std::chrono::steady_clock::time_point next_reconnect_;
  std::chrono::steady_clock::time_point connect_deadline_;

  bool chat_active_ = false;
  std::string chat_input_;

  TokenId selected_id_ = 0;
  bool dragging_ = false;
  std::optional<Cell> drag_target_;
  float mouse_x_ = 0.0F;
  float mouse_y_ = 0.0F;
};

}  // namespace dnd
