#pragma once

#include <SDL3/SDL.h>
#include <SDL3_ttf/SDL_ttf.h>

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace dnd {

struct TextMetrics {
  float width = 0.0F;
  float height = 0.0F;
};

// Owns a TTF text engine and a reusable pool of TTF_Text objects. Call
// BeginFrame() once per rendered frame before issuing draw calls.
class TextRenderer {
 public:
  TextRenderer(SDL_Renderer* renderer, TTF_Font* font);
  ~TextRenderer();

  TextRenderer(const TextRenderer&) = delete;
  auto operator=(const TextRenderer&) -> TextRenderer& = delete;

  [[nodiscard]] auto Valid() const -> bool;
  [[nodiscard]] auto LineHeight() const -> float;

  auto BeginFrame() -> void;

  // Single-line text. max_width > 0 enables ellipsis when the text does not
  // fit. Pass 0 for no limit.
  auto Draw(std::string_view text, float x, float y, SDL_Color color,
            float max_width = 0.0F) -> void;

  // Word-wrapped text. Returns the y coordinate below the drawn block.
  auto DrawWrapped(std::string_view text, float x, float y, SDL_Color color,
                   float max_width) -> float;

  auto DrawCentered(std::string_view text, const SDL_FRect& bounds,
                    SDL_Color color) -> void;

  [[nodiscard]] auto Measure(std::string_view text, float wrap_width = 0.0F)
      -> TextMetrics;

 private:
  struct Slot {
    TTF_Text* text = nullptr;
    std::string content;
    int wrap_width = -1;
    SDL_Color color{};
    bool color_set = false;
  };

  auto Acquire() -> Slot&;
  auto Update(Slot& slot, std::string_view text, int wrap_width,
              SDL_Color color) -> void;
  auto MeasureText(TTF_Text* text) -> TextMetrics;

  SDL_Renderer* renderer_ = nullptr;
  TTF_Font* font_ = nullptr;
  TTF_TextEngine* engine_ = nullptr;
  std::vector<Slot> slots_;
  std::size_t next_slot_ = 0;
  TTF_Text* measure_text_ = nullptr;
};

}  // namespace dnd
