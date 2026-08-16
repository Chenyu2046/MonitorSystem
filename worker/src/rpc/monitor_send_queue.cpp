#include "rpc/monitor_send_queue.h"

#include <algorithm>
#include <utility>

namespace monitor {

MonitorSendQueue::MonitorSendQueue(std::size_t max_items, std::size_t max_bytes)
    : max_items_(std::max<std::size_t>(1, max_items)),
      max_bytes_(std::max<std::size_t>(1, max_bytes)) {}

void MonitorSendQueue::Open() {
  std::lock_guard<std::mutex> lock(mutex_);
  closed_ = false;
}

bool MonitorSendQueue::Push(monitor::proto::MonitorInfo info) {
  const std::size_t bytes = info.ByteSizeLong();
  const bool priority = IsPriority(info);
  std::lock_guard<std::mutex> lock(mutex_);
  if (closed_ || bytes > max_bytes_) {
    ++dropped_count_;
    return false;
  }

  while (!HasCapacity(bytes)) {
    if (DropOldestBaseMetric()) {
      continue;
    }

    if (!priority || items_.empty()) {
      ++dropped_count_;
      return false;
    }

    bytes_ -= items_.front().bytes;
    items_.pop_front();
    ++dropped_count_;
  }

  bytes_ += bytes;
  items_.push_back(Item{std::move(info), bytes, priority});
  condition_.notify_one();
  return true;
}

bool MonitorSendQueue::Pop(monitor::proto::MonitorInfo* info) {
  std::unique_lock<std::mutex> lock(mutex_);
  condition_.wait(lock, [this] { return closed_ || !items_.empty(); });
  if (items_.empty()) {
    return false;
  }

  Item item = std::move(items_.front());
  items_.pop_front();
  bytes_ -= item.bytes;
  *info = std::move(item.info);
  return true;
}

void MonitorSendQueue::Close() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    closed_ = true;
  }
  condition_.notify_all();
}

std::size_t MonitorSendQueue::size() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return items_.size();
}

std::uint64_t MonitorSendQueue::dropped_count() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return dropped_count_;
}

bool MonitorSendQueue::IsPriority(const monitor::proto::MonitorInfo& info) {
  if (!info.has_diagnostic()) {
    return false;
  }
  const auto& diagnostic = info.diagnostic();
  return diagnostic.state() != monitor::proto::OBSERVABILITY_NORMAL ||
         diagnostic.anomaly_score() >= 0.6 ||
         diagnostic.oncpu_profiles_size() > 0 ||
         diagnostic.offcpu_profiles_size() > 0;
}

bool MonitorSendQueue::HasCapacity(std::size_t bytes) const {
  return items_.size() < max_items_ && bytes <= max_bytes_ - bytes_;
}

bool MonitorSendQueue::DropOldestBaseMetric() {
  const auto item =
      std::find_if(items_.begin(), items_.end(),
                   [](const Item& queued) { return !queued.priority; });
  if (item == items_.end()) {
    return false;
  }
  bytes_ -= item->bytes;
  items_.erase(item);
  ++dropped_count_;
  return true;
}

}  // namespace monitor
