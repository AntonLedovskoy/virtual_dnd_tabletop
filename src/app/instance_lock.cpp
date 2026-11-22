#include "app/instance_lock.h"

#include <array>
#include <cstdint>
#include <string>
#include <system_error>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace dnd {
namespace {

#ifdef _WIN32
HANDLE g_instance_mutex = nullptr;

// FNV-1a по нормализованному пути: имя мьютекса не должно зависеть от
// регистра и `..` в командной строке.
auto PathHash(const std::filesystem::path& path) -> std::uint64_t {
  std::error_code code;
  std::filesystem::path normalized =
      std::filesystem::weakly_canonical(path, code);
  if (code) {
    normalized = path;
  }
  const std::u8string text = normalized.u8string();
  std::uint64_t hash = 1469598103934665603ULL;
  for (const char8_t byte : text) {
    hash ^= static_cast<std::uint64_t>(byte);
    hash *= 1099511628211ULL;
  }
  return hash;
}
#endif

}  // namespace

auto AcquireInstanceLock(const std::filesystem::path& base_path) -> bool {
#ifdef _WIN32
  ReleaseInstanceLock();
  std::wstring name = L"Local\\virtual_dnd_tabletop_";
  name += std::to_wstring(PathHash(base_path));
  g_instance_mutex = CreateMutexW(nullptr, FALSE, name.c_str());
  if (g_instance_mutex == nullptr) {
    // Не блокируем игру из-за прав на объект ядра: это хуже, чем два окна.
    return true;
  }
  if (GetLastError() == ERROR_ALREADY_EXISTS) {
    CloseHandle(g_instance_mutex);
    g_instance_mutex = nullptr;
    return false;
  }
  return true;
#else
  (void)base_path;
  return true;
#endif
}

auto ReleaseInstanceLock() -> void {
#ifdef _WIN32
  if (g_instance_mutex != nullptr) {
    CloseHandle(g_instance_mutex);
    g_instance_mutex = nullptr;
  }
#endif
}

}  // namespace dnd
