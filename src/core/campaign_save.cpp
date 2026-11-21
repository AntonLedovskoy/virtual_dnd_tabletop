#include "core/campaign_save.h"

#include <utility>

#include "core/campaign_io.h"

namespace dnd {

CampaignSaver::CampaignSaver() : worker_([this]() -> void { Run(); }) {}

CampaignSaver::~CampaignSaver() {
  {
    const std::scoped_lock lock(mutex_);
    stopping_ = true;
  }
  has_work_.notify_all();
  if (worker_.joinable()) {
    worker_.join();
  }
}

auto CampaignSaver::Request(Campaign campaign, std::filesystem::path path)
    -> void {
  {
    const std::scoped_lock lock(mutex_);
    pending_ = Job{.campaign = std::move(campaign), .path = std::move(path)};
  }
  has_work_.notify_one();
}

auto CampaignSaver::Busy() const -> bool {
  const std::scoped_lock lock(mutex_);
  return running_ || pending_.has_value();
}

auto CampaignSaver::TakeResult() -> std::optional<bool> {
  const std::scoped_lock lock(mutex_);
  const std::optional<bool> result = result_;
  result_.reset();
  return result;
}

auto CampaignSaver::Flush() -> bool {
  std::unique_lock<std::mutex> lock(mutex_);
  idle_.wait(lock,
             [this]() -> bool { return !running_ && !pending_.has_value(); });
  const bool ok = result_.value_or(true);
  result_.reset();
  return ok;
}

auto CampaignSaver::Run() -> void {
  for (;;) {
    Job job;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      has_work_.wait(
          lock, [this]() -> bool { return stopping_ || pending_.has_value(); });
      if (!pending_.has_value()) {
        // Остановка: очередь пуста, незавершённых просьб не осталось.
        return;
      }
      job = std::move(*pending_);
      pending_.reset();
      running_ = true;
    }
    const bool ok = WriteCampaign(job.campaign, job.path);
    {
      const std::scoped_lock lock(mutex_);
      running_ = false;
      result_ = ok;
    }
    idle_.notify_all();
  }
}

}  // namespace dnd
