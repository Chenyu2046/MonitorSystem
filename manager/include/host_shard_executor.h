#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "concurrency/bounded_queue.h"
#include "data_receive_result.h"
#include "monitor_info.pb.h"

namespace monitor {

class HostShardExecutor {
 public:
  using ProcessCallback = std::function<void(
      std::size_t shard_id, const std::string& host_name,
      const monitor::proto::MonitorInfo& info,
      std::chrono::system_clock::time_point received_at,
      std::chrono::steady_clock::time_point enqueued_at)>;

  HostShardExecutor(std::size_t shard_count, std::size_t queue_capacity,
                    std::size_t queue_max_bytes, ProcessCallback callback);
  ~HostShardExecutor();

  HostShardExecutor(const HostShardExecutor&) = delete;
  HostShardExecutor& operator=(const HostShardExecutor&) = delete;

  void Start();
  void Stop();

  DataReceiveResult Submit(
      const std::string& host_name, monitor::proto::MonitorInfo info);

  std::size_t ShardFor(const std::string& host_name) const;
  std::size_t shard_count() const { return shards_.size(); }
  std::size_t PeakQueueDepth() const;
  std::size_t PeakQueueBytes() const;

 private:
  struct WorkItem {
    std::string host_name;
    monitor::proto::MonitorInfo info;
    std::chrono::system_clock::time_point received_at;
    std::chrono::steady_clock::time_point enqueued_at;
  };

  struct Shard {
    Shard(std::size_t capacity, std::size_t max_bytes)
        : queue(capacity, max_bytes, [](const WorkItem& item) {
            return sizeof(WorkItem) + item.host_name.size() +
                   item.info.ByteSizeLong();
          }) {}

    concurrency::BoundedQueue<WorkItem> queue;
    std::thread worker;
  };

  void RunShard(std::size_t shard_id);

  const ProcessCallback callback_;
  std::vector<std::unique_ptr<Shard>> shards_;
  std::atomic<bool> accepting_{false};
  bool started_ = false;
};

}  // namespace monitor
