#pragma once

#include <cstdio>
#include <string>

#include "core/log_line.h"

namespace dnd {

// Строка файла сессии: «2026-11-20 18:40:12 автор: текст». Автор может быть
// пустым — тогда пишется только текст. Отдельной функцией, чтобы формат
// можно было проверить тестом.
auto FormatSessionLogLine(const LogLine& line) -> std::string;

// Дописывает общий лог стола в файл рядом с игрой. Файл открывается на
// дозапись: прошлые сессии не теряются, перед новыми строками ставится
// заголовок. Ошибки записи не ломают игру — лог просто не пишется.
class SessionLog {
 public:
  SessionLog() = default;
  ~SessionLog();

  SessionLog(const SessionLog&) = delete;
  auto operator=(const SessionLog&) -> SessionLog& = delete;

  // Открывает файл и пишет заголовок сессии с временем started_at_unix.
  auto Open(const std::string& path, std::uint32_t started_at_unix) -> bool;
  auto Append(const LogLine& line) -> void;
  // Сбрасывает буфер на диск; полезно перед выходом и после пачки строк.
  auto Flush() -> void;
  auto Close() -> void;

  [[nodiscard]] auto IsOpen() const -> bool { return file_ != nullptr; }
  [[nodiscard]] auto Path() const -> const std::string& { return path_; }

 private:
  std::FILE* file_ = nullptr;
  std::string path_;
};

}  // namespace dnd
