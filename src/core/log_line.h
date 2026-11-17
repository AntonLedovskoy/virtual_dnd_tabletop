#pragma once

#include <cstdint>
#include <string>

namespace dnd {

// Одна строка общего лога: чат, броски кубов и системные сообщения.
// timestamp — секунды Unix; 0 означает «неизвестно» и выводится без времени.
struct LogLine {
  std::string author;
  std::string text;
  std::uint32_t timestamp = 0;
};

}  // namespace dnd
