#pragma once

#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <optional>
#include <thread>

#include "core/campaign.h"

namespace dnd {

// Запись кампании в отдельном потоке: кадр не ждёт диск, поэтому автосейв
// растущей кампании (до kMaxCampaignScenes сцен) не даёт пауз в игре.
// Копия мира снимается в момент Request, так что играть можно сразу.
class CampaignSaver {
 public:
  CampaignSaver();
  ~CampaignSaver();
  CampaignSaver(const CampaignSaver&) = delete;
  auto operator=(const CampaignSaver&) -> CampaignSaver& = delete;

  // Новая просьба заменяет ещё не начатую: на диск уйдёт самый свежий мир.
  // Уже идущая запись дописывается, следом идёт эта.
  auto Request(Campaign campaign, std::filesystem::path path) -> void;
  // Есть незаписанная кампания или идёт запись.
  [[nodiscard]] auto Busy() const -> bool;
  // Результат записи, завершившейся с прошлого опроса; nullopt — нечего.
  auto TakeResult() -> std::optional<bool>;
  // Дождаться диска (выход, явное сохранение): true — последняя запись удалась.
  auto Flush() -> bool;

 private:
  struct Job {
    Campaign campaign;
    std::filesystem::path path;
  };

  auto Run() -> void;

  mutable std::mutex mutex_;
  // Будит рабочий поток при новой просьбе и при остановке.
  std::condition_variable has_work_;
  // Будит Flush, когда очередь опустела.
  std::condition_variable idle_;
  std::optional<Job> pending_;
  std::optional<bool> result_;
  bool running_ = false;
  bool stopping_ = false;
  std::thread worker_;
};

}  // namespace dnd
