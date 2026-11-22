#pragma once

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>

#include "core/update.h"
#include "core/version.h"

namespace dnd {

// Проверка обновлений идёт в фоновом потоке: кадр не ждёт сеть и не знает,
// что она есть. Состояния — как в PLAN 7.3.
enum class UpdateStage : std::uint8_t {
  kIdle = 0,
  kChecking = 1,
  kUpdateAvailable = 2,
  kDownloading = 3,
  kReady = 4,
  kApplying = 5,
  kFailed = 6,
};

struct UpdateStatus {
  UpdateStage stage = UpdateStage::kIdle;
  UpdateManifest manifest;
  // 0..100 во время скачивания; 100 — файл проверен и лежит рядом с exe.
  int progress = 0;
  // Папка игры только для чтения (Program Files): подменить exe нельзя,
  // остаётся скачать его вручную по ссылке из манифеста.
  bool manual_only = false;
  // Текст ошибки для баннера; пусто — всё хорошо.
  std::string error;
};

// Скачивает `latest.json`, решает, есть ли обновление, и по просьбе качает
// exe с проверкой sha256. Сеть — WinHTTP, только в exe-цели: `dnd_core`
// остаётся без сокетов и SDL.
class UpdateService {
 public:
  UpdateService(std::filesystem::path base_path, Version current, int protocol,
                bool enabled, std::string manifest_override);
  ~UpdateService();

  UpdateService(const UpdateService&) = delete;
  auto operator=(const UpdateService&) -> UpdateService& = delete;

  // Запуск проверки. Пока идёт проверка или скачивание, повторная просьба
  // игнорируется: поток один.
  auto StartCheck() -> void;
  // Скачивание уже найденного обновления (из состояния kUpdateAvailable или
  // kFailed).
  auto StartDownload() -> void;
  // Подмена началась: баннер больше не нужен, игра сейчас выйдет.
  auto MarkApplying() -> void;
  // Подмена или запись не удались: показать текст в баннере.
  auto Fail(std::string error) -> void;

  [[nodiscard]] auto Status() const -> UpdateStatus;
  [[nodiscard]] auto BasePath() const -> const std::filesystem::path&;

 private:
  auto JoinWorker() -> void;
  auto RunCheck() -> void;
  auto RunDownload() -> void;
  auto SetStage(UpdateStage stage, std::string error = {}) -> void;
  auto SetProgress(int progress) -> void;

  std::filesystem::path base_path_;
  // Откуда читать манифест: пусто — стабильная ссылка GitHub, иначе файл
  // или URL из DND_UPDATE_MANIFEST.
  std::string manifest_override_;
  std::filesystem::path manifest_directory_;
  Version current_;
  int protocol_ = 0;
  bool enabled_ = false;

  mutable std::mutex mutex_;
  UpdateStatus status_;
  std::thread worker_;
};

// Подмена exe в меню: `virtual_dnd_tabletop.exe` → `.old.exe`,
// `.update.exe` → `virtual_dnd_tabletop.exe`, запуск нового процесса.
// false — подмена не удалась, error объясняет что делать.
auto InstallDownloadedUpdate(const std::filesystem::path& base_path,
                             std::string& error) -> bool;

}  // namespace dnd
