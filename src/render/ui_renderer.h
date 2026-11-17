#pragma once

#include <string>
#include <vector>

#include "core/encounter.h"
#include "core/log_line.h"
#include "core/token.h"

struct SDL_Renderer;

namespace dnd {

class TextRenderer;

// Bottom overlay with the shared table log and the chat input line.
auto DrawLogPanel(SDL_Renderer* renderer, TextRenderer* text,
                  const std::vector<LogLine>& lines, bool typing,
                  const std::string& input) -> void;

// Top-right initiative order of the tokens the client can see.
auto DrawInitiativePanel(SDL_Renderer* renderer, TextRenderer* text,
                         const std::vector<Token>& tokens,
                         const Encounter& encounter) -> void;

// Top-left details for the selected token, including claim/owner hints.
// Returns the bottom edge of the panel, or 0 when nothing was drawn.
auto DrawTokenPanel(SDL_Renderer* renderer, TextRenderer* text,
                    const Token* token, const std::string& player_name,
                    bool is_client) -> float;

// Left-side player list from the host roster, placed at the requested y.
auto DrawPlayersPanel(SDL_Renderer* renderer, TextRenderer* text,
                      const std::vector<std::string>& players, float panel_y)
    -> void;

}  // namespace dnd
