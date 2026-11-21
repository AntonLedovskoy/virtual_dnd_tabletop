#include "core/player_name.h"

#include <cstddef>
#include <cstdint>

namespace dnd {
namespace {

struct DecodedCodePoint {
  char32_t code = 0;
  // Битая UTF-8 последовательность: код — просто первый байт.
  bool valid = true;
};

// Читает одну кодовую точку, сдвигая index. Некорректная последовательность
// пропускается побайтно, чтобы сравнение имён не зависело от валидности.
auto DecodeCodePoint(std::string_view text, std::size_t& index)
    -> DecodedCodePoint {
  const auto first = static_cast<unsigned char>(text[index]);
  if (first < 0x80U) {
    ++index;
    return DecodedCodePoint{.code = first};
  }
  std::size_t length = 0;
  char32_t code = 0;
  if ((first & 0xE0U) == 0xC0U) {
    length = 2;
    code = first & 0x1FU;
  } else if ((first & 0xF0U) == 0xE0U) {
    length = 3;
    code = first & 0x0FU;
  } else if ((first & 0xF8U) == 0xF0U) {
    length = 4;
    code = first & 0x07U;
  } else {
    ++index;
    return DecodedCodePoint{.code = first, .valid = false};
  }
  if (index + length > text.size()) {
    ++index;
    return DecodedCodePoint{.code = first, .valid = false};
  }
  for (std::size_t offset = 1; offset < length; ++offset) {
    const auto next = static_cast<unsigned char>(text[index + offset]);
    if ((next & 0xC0U) != 0x80U) {
      ++index;
      return DecodedCodePoint{.code = first, .valid = false};
    }
    code = (code << 6) | (next & 0x3FU);
  }
  index += length;
  return DecodedCodePoint{.code = code};
}

// Простое понижение регистра: латиница, латиница-1, греческий, кириллица.
auto FoldCodePoint(char32_t code) -> char32_t {
  if (code >= U'A' && code <= U'Z') {
    return code + 32;
  }
  // À-Ö и Ø-Þ.
  if ((code >= 0x00C0 && code <= 0x00D6) ||
      (code >= 0x00D8 && code <= 0x00DE)) {
    return code + 32;
  }
  // Латинский расширенный-A: прописные идут парами с шагом 1, но чётность
  // в блоках разная из-за одиночных U+0138 и U+0149.
  const bool upper_extended =
      (code >= 0x0100 && code <= 0x0137 && (code % 2) == 0) ||
      (code >= 0x0139 && code <= 0x0148 && (code % 2) == 1) ||
      (code >= 0x014A && code <= 0x0177 && (code % 2) == 0) ||
      (code >= 0x0179 && code <= 0x017E && (code % 2) == 1);
  if (upper_extended) {
    return code + 1;
  }
  if (code == 0x0178) {
    return 0x00FF;  // Ÿ -> ÿ
  }
  if (code == 0x017F) {
    return U's';  // ſ -> s
  }
  // Α-Ω; U+03A2 не занят, поэтому диапазон берётся целиком.
  if (code >= 0x0391 && code <= 0x03A9) {
    return code + 32;
  }
  // Конечная сигма считается той же буквой, что и обычная.
  if (code == 0x03C2) {
    return 0x03C3;
  }
  // А-Я.
  if (code >= 0x0410 && code <= 0x042F) {
    return code + 32;
  }
  // Ѐ-Џ: строчные на 0x50 выше.
  if (code >= 0x0400 && code <= 0x040F) {
    return code + 80;
  }
  return code;
}

auto AppendCodePoint(std::string& out, char32_t code) -> void {
  if (code <= 0x7F) {
    out.push_back(static_cast<char>(code));
  } else if (code <= 0x7FF) {
    out.push_back(static_cast<char>(0xC0U | (code >> 6)));
    out.push_back(static_cast<char>(0x80U | (code & 0x3FU)));
  } else if (code <= 0xFFFF) {
    out.push_back(static_cast<char>(0xE0U | (code >> 12)));
    out.push_back(static_cast<char>(0x80U | ((code >> 6) & 0x3FU)));
    out.push_back(static_cast<char>(0x80U | (code & 0x3FU)));
  } else {
    out.push_back(static_cast<char>(0xF0U | (code >> 18)));
    out.push_back(static_cast<char>(0x80U | ((code >> 12) & 0x3FU)));
    out.push_back(static_cast<char>(0x80U | ((code >> 6) & 0x3FU)));
    out.push_back(static_cast<char>(0x80U | (code & 0x3FU)));
  }
}

}  // namespace

auto PlayerNameKey(std::string_view name) -> std::string {
  std::string key;
  key.reserve(name.size());
  std::size_t index = 0;
  while (index < name.size()) {
    const DecodedCodePoint decoded = DecodeCodePoint(name, index);
    if (!decoded.valid) {
      // Битая последовательность сравнивается как есть.
      key.push_back(name[index - 1]);
      continue;
    }
    AppendCodePoint(key, FoldCodePoint(decoded.code));
  }
  return key;
}

auto SamePlayerName(std::string_view left, std::string_view right) -> bool {
  std::size_t left_index = 0;
  std::size_t right_index = 0;
  while (left_index < left.size() && right_index < right.size()) {
    const DecodedCodePoint a = DecodeCodePoint(left, left_index);
    const DecodedCodePoint b = DecodeCodePoint(right, right_index);
    if (a.valid != b.valid) {
      return false;
    }
    if (a.valid) {
      if (FoldCodePoint(a.code) != FoldCodePoint(b.code)) {
        return false;
      }
    } else if (a.code != b.code) {
      return false;
    }
  }
  return left_index >= left.size() && right_index >= right.size();
}

}  // namespace dnd
