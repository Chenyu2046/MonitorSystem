#pragma once

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>

#include "monitor_info.pb.h"

namespace monitor {

class MonitorSendQueue {
 public:
  MonitorSendQueue(std::size_t max_items, std::size_t max_bytes);

  void Open();
  bool Push(monitor::proto::MonitorInfo info);
  bool Pop(monitor::proto::MonitorInfo* info);
  void Close();

  std::size_t size() const;
  std::uint64_t dropped_count() const;

 private:
  struct Item {
    monitor::proto::MonitorInfo info;
    std::size_t bytes = 0;
    bool priority = false;
  };

  static bool IsPriority(const monitor::proto::MonitorInfo& info);
  bool HasCapacity(std::size_t bytes) const;
  bool DropOldestBaseMetric();

  const std::size_t max_items_;
  const std::size_t max_bytes_;
  mutable std::mutex mutex_;
  std::condition_variable condition_;
  std::deque<Item> items_;
  std::size_t bytes_ = 0;
  std::uint64_t dropped_count_ = 0;
  bool closed_ = true;
};

}  // namespace monitor
