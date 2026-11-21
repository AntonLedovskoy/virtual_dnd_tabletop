#include "app/file_log.h"

#include <SDL3/SDL.h>

#include <array>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <mutex>
#include <string>

namespace dnd {
namespace {

// SDL_Log можно вызывать из сетевого потока, поэтому файл защищён мьютексом.
std::mutex g_log_mutex;
std::FILE* g_log_file = nullptr;
SDL_LogOutputFunction g_default_output = nullptr;

// Локальное время строки журнала: «2026-11-21 14:05:12.123».
auto TimeText() -> std::string {
  const auto now = std::chrono::system_clock::now();
  const auto stamp = std::chrono::system_clock::to_time_t(now);
  const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(
                          now.time_since_epoch()) %
                      1000;
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
  std::array<char, 48> buffer{};
  const int written = std::snprintf(
      buffer.data(), buffer.size(), "%04d-%02d-%02d %02d:%02d:%02d.%03d",
      parts.tm_year + 1900, parts.tm_mon + 1, parts.tm_mday, parts.tm_hour,
      parts.tm_min, parts.tm_sec, static_cast<int>(millis.count()));
  if (written <= 0) {
    return {};
  }
  return {buffer.data(), static_cast<std::size_t>(written)};
}

auto PriorityName(SDL_LogPriority priority) -> const char* {
  switch (priority) {
    case SDL_LOG_PRIORITY_TRACE:
      return "TRACE";
    case SDL_LOG_PRIORITY_VERBOSE:
      return "VERBOSE";
    case SDL_LOG_PRIORITY_DEBUG:
      return "DEBUG";
    case SDL_LOG_PRIORITY_INFO:
      return "INFO";
    case SDL_LOG_PRIORITY_WARN:
      return "WARN";
    case SDL_LOG_PRIORITY_ERROR:
      return "ERROR";
    case SDL_LOG_PRIORITY_CRITICAL:
      return "CRITICAL";
    default:
      return "LOG";
  }
}

auto WriteLine(const std::string& line) -> void {
  const std::scoped_lock lock(g_log_mutex);
  if (g_log_file == nullptr) {
    return;
  }
  (void)std::fwrite(line.data(), 1, line.size(), g_log_file);
  // Строки пишутся редко, поэтому сброс после каждой: журнал переживает
  // падение и не теряет хвост при завершении процесса.
  std::fflush(g_log_file);
}

void SDLCALL AppendToFile(void* /*userdata*/, int category,
                          SDL_LogPriority priority, const char* message) {
  // Стандартный вывод SDL не теряется: в отладчике и в консоли строка
  // видна как раньше, когда она есть.
  if (g_default_output != nullptr) {
    g_default_output(nullptr, category, priority, message);
  }
  std::string line = "[";
  line += TimeText();
  line += "] ";
  line += PriorityName(priority);
  line += " ";
  if (message != nullptr) {
    line += message;
  }
  line += '\n';
  WriteLine(line);
}

}  // namespace

auto InstallFileLog() -> void {
  const std::scoped_lock lock(g_log_mutex);
  if (g_log_file != nullptr) {
    return;
  }
  std::filesystem::path path = kFileLogName;
  if (const char* base = SDL_GetBasePath()) {
    path = std::filesystem::path(base) / kFileLogName;
  }
  // Дозапись без стирания прошлых запусков: причину падения ищут после него.
  g_log_file = std::fopen(path.string().c_str(), "ab");
  if (g_log_file == nullptr) {
    return;
  }
  const std::string header = "\n=== запуск " + TimeText() + " ===\n";
  (void)std::fwrite(header.data(), 1, header.size(), g_log_file);
  std::fflush(g_log_file);
  g_default_output = SDL_GetDefaultLogOutputFunction();
  SDL_SetLogOutputFunction(AppendToFile, nullptr);
}

auto ShutdownFileLog() -> void {
  if (g_default_output != nullptr) {
    SDL_SetLogOutputFunction(g_default_output, nullptr);
  }
  const std::scoped_lock lock(g_log_mutex);
  if (g_log_file != nullptr) {
    std::fflush(g_log_file);
    std::fclose(g_log_file);
    g_log_file = nullptr;
  }
  g_default_output = nullptr;
}

}  // namespace dnd
