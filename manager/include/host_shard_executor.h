#pragma once

#include <atomic>
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
      const monitor::proto::MonitorInfo& info)>;

  HostShardExecutor(std::size_t shard_count, std::size_t queue_capacity,
                    ProcessCallback callback);
  ~HostShardExecutor();

  HostShardExecutor(const HostShardExecutor&) = delete;
  HostShardExecutor& operator=(const HostShardExecutor&) = delete;

  void Start();
  void Stop();

  DataReceiveResult Submit(
      const std::string& host_name, monitor::proto::MonitorInfo info);

  std::size_t ShardFor(const std::string& host_name) const;
  std::size_t shard_count() const { return shards_.size(); }

 private:
  struct WorkItem {
    std::string host_name;
    monitor::proto::MonitorInfo info;
  };

  struct Shard {
    explicit Shard(std::size_t capacity) : queue(capacity) {}

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
