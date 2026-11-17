#include "render/text_renderer.h"

#include <algorithm>
#include <utility>

#include "core/utf8.h"

namespace dnd {
namespace {

auto SameColor(SDL_Color left, SDL_Color right) -> bool {
  return left.r == right.r && left.g == right.g && left.b == right.b &&
         left.a == right.a;
}

}  // namespace

TextRenderer::TextRenderer(SDL_Renderer* renderer, TTF_Font* font)
    : renderer_(renderer), font_(font) {
  if (renderer_ != nullptr && font_ != nullptr) {
    engine_ = TTF_CreateRendererTextEngine(renderer_);
  }
  if (engine_ != nullptr) {
    measure_text_ = TTF_CreateText(engine_, font_, "", 0);
  }
}

TextRenderer::~TextRenderer() {
  if (measure_text_ != nullptr) {
    TTF_DestroyText(measure_text_);
    measure_text_ = nullptr;
  }
  for (Slot& slot : slots_) {
    if (slot.text != nullptr) {
      TTF_DestroyText(slot.text);
      slot.text = nullptr;
    }
  }
  slots_.clear();
  if (engine_ != nullptr) {
    TTF_DestroyRendererTextEngine(engine_);
    engine_ = nullptr;
  }
}

auto TextRenderer::Valid() const -> bool {
  return engine_ != nullptr && font_ != nullptr && measure_text_ != nullptr;
}

auto TextRenderer::LineHeight() const -> float {
  return font_ != nullptr ? static_cast<float>(TTF_GetFontHeight(font_)) : 0.0F;
}

auto TextRenderer::BeginFrame() -> void { next_slot_ = 0; }

auto TextRenderer::Acquire() -> Slot& {
  if (next_slot_ < slots_.size()) {
    return slots_[next_slot_++];
  }
  Slot slot;
  slot.text = TTF_CreateText(engine_, font_, "", 0);
  slots_.push_back(std::move(slot));
  ++next_slot_;
  return slots_.back();
}

auto TextRenderer::Update(Slot& slot, std::string_view text, int wrap_width,
                          SDL_Color color) -> void {
  if (slot.text == nullptr) {
    return;
  }
  if (slot.content.size() != text.size() ||
      slot.content.compare(0, slot.content.size(), text.data(), text.size()) !=
          0) {
    TTF_SetTextString(slot.text, text.data(), text.size());
    slot.content.assign(text.data(), text.size());
  }
  if (slot.wrap_width != wrap_width) {
    TTF_SetTextWrapWidth(slot.text, wrap_width);
    slot.wrap_width = wrap_width;
  }
  if (!slot.color_set || !SameColor(slot.color, color)) {
    TTF_SetTextColor(slot.text, color.r, color.g, color.b, color.a);
    slot.color = color;
    slot.color_set = true;
  }
}

auto TextRenderer::MeasureText(TTF_Text* text) -> TextMetrics {
  int width = 0;
  int height = 0;
  if (text == nullptr || !TTF_GetTextSize(text, &width, &height)) {
    return {};
  }
  return TextMetrics{.width = static_cast<float>(width),
                     .height = static_cast<float>(height)};
}

auto TextRenderer::Draw(std::string_view text, float x, float y,
                        SDL_Color color, float max_width) -> void {
  if (!Valid() || text.empty()) {
    return;
  }
  Slot& slot = Acquire();
  Update(slot, text, 0, color);
  TextMetrics metrics = MeasureText(slot.text);
  if (max_width > 0.0F && metrics.width > max_width) {
    std::string shortened(text);
    while (!shortened.empty()) {
      shortened.resize(PreviousCodePointStart(shortened));
      const std::string candidate = shortened + "...";
      Update(slot, candidate, 0, color);
      metrics = MeasureText(slot.text);
      if (metrics.width <= max_width) {
        break;
      }
    }
  }
  if (slot.text != nullptr) {
    TTF_DrawRendererText(slot.text, x, y);
  }
}

auto TextRenderer::DrawWrapped(std::string_view text, float x, float y,
                               SDL_Color color, float max_width) -> float {
  if (!Valid() || text.empty()) {
    return y;
  }
  const int wrap_width =
      max_width > 0.0F ? std::max(1, static_cast<int>(max_width)) : 0;
  Slot& slot = Acquire();
  Update(slot, text, wrap_width, color);
  const TextMetrics metrics = MeasureText(slot.text);
  if (slot.text != nullptr) {
    TTF_DrawRendererText(slot.text, x, y);
  }
  return y + metrics.height;
}

auto TextRenderer::DrawCentered(std::string_view text, const SDL_FRect& bounds,
                                SDL_Color color) -> void {
  if (!Valid() || text.empty()) {
    return;
  }
  Slot& slot = Acquire();
  Update(slot, text, 0, color);
  const TextMetrics metrics = MeasureText(slot.text);
  const float x = bounds.x + ((bounds.w - metrics.width) / 2.0F);
  const float y = bounds.y + ((bounds.h - metrics.height) / 2.0F);
  if (slot.text != nullptr) {
    TTF_DrawRendererText(slot.text, x, y);
  }
}

auto TextRenderer::Measure(std::string_view text, float wrap_width)
    -> TextMetrics {
  if (!Valid() || text.empty()) {
    return {};
  }
  TTF_SetTextString(measure_text_, text.data(), text.size());
  const int width =
      wrap_width > 0.0F ? std::max(1, static_cast<int>(wrap_width)) : 0;
  TTF_SetTextWrapWidth(measure_text_, width);
  return MeasureText(measure_text_);
}

}  // namespace dnd
