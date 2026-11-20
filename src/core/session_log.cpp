#include "core/session_log.h"

#include <array>
#include <ctime>

namespace dnd {
namespace {

// Локальное время сессии; при неизвестном времени (0) поле пустое.
auto TimeText(std::uint32_t seconds) -> std::string {
  if (seconds == 0) {
    return {};
  }
  const auto stamp = static_cast<std::time_t>(seconds);
  std::tm parts{};
#ifdef _WIN32
  if (localtime_s(&parts, &stamp) != 0) {
    return {};
  }
#else
  if (localtime_r(&stamp, &parts) == nullptr) {
    return {};
  }
#endif
  std::array<char, 32> buffer{};
  const int written = std::snprintf(
      buffer.data(), buffer.size(), "%04d-%02d-%02d %02d:%02d:%02d",
      parts.tm_year + 1900, parts.tm_mon + 1, parts.tm_mday, parts.tm_hour,
      parts.tm_min, parts.tm_sec);
  if (written <= 0) {
    return {};
  }
  return {buffer.data(), static_cast<std::size_t>(written)};
}

}  // namespace

auto FormatSessionLogLine(const LogLine& line) -> std::string {
  std::string text;
  const std::string time = TimeText(line.timestamp);
  if (!time.empty()) {
    text += "[";
    text += time;
    text += "] ";
  }
  if (!line.author.empty()) {
    text += line.author;
    text += ": ";
  }
  text += line.text;
  text += '\n';
  return text;
}

SessionLog::~SessionLog() { Close(); }

auto SessionLog::Open(const std::string& path, std::uint32_t started_at_unix)
    -> bool {
  Close();
  if (path.empty()) {
    return false;
  }
  // Дозапись без стирания прошлых сессий: разбор после игры не должен
  // зависеть от того, когда её начали.
  file_ = std::fopen(path.c_str(), "ab");
  if (file_ == nullptr) {
    return false;
  }
  path_ = path;
  std::string header = "\n=== сессия ";
  const std::string time = TimeText(started_at_unix);
  header += time.empty() ? std::string("без времени") : time;
  header += " ===\n";
  (void)std::fwrite(header.data(), 1, header.size(), file_);
  std::fflush(file_);
  return true;
}

auto SessionLog::Append(const LogLine& line) -> void {
  if (file_ == nullptr) {
    return;
  }
  const std::string text = FormatSessionLogLine(line);
  (void)std::fwrite(text.data(), 1, text.size(), file_);
}

auto SessionLog::Flush() -> void {
  if (file_ != nullptr) {
    std::fflush(file_);
  }
}

auto SessionLog::Close() -> void {
  if (file_ != nullptr) {
    std::fflush(file_);
    std::fclose(file_);
    file_ = nullptr;
  }
  path_.clear();
}

}  // namespace dnd
