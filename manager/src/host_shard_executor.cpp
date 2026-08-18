#include "host_shard_executor.h"

#include <algorithm>
#include <chrono>
#include <functional>
#include <utility>

namespace monitor {

HostShardExecutor::HostShardExecutor(std::size_t shard_count,
                                     std::size_t queue_capacity,
                                     std::size_t queue_max_bytes,
                                     ProcessCallback callback)
    : callback_(std::move(callback)) {
  if (shard_count == 0) {
    shard_count = 1;
  }
  shards_.reserve(shard_count);
  for (std::size_t index = 0; index < shard_count; ++index) {
    shards_.push_back(
        std::make_unique<Shard>(queue_capacity, queue_max_bytes));
  }
}

HostShardExecutor::~HostShardExecutor() { Stop(); }

void HostShardExecutor::Start() {
  if (started_) {
    return;
  }
  started_ = true;
  accepting_.store(true, std::memory_order_release);
  for (std::size_t index = 0; index < shards_.size(); ++index) {
    shards_[index]->worker = std::thread(&HostShardExecutor::RunShard, this,
                                         index);
  }
}

void HostShardExecutor::Stop() {
  accepting_.store(false, std::memory_order_release);
  if (!started_) {
    return;
  }
  for (const auto& shard : shards_) {
    shard->queue.Close();
  }
  for (const auto& shard : shards_) {
    if (shard->worker.joinable()) {
      shard->worker.join();
    }
  }
  started_ = false;
}

DataReceiveResult HostShardExecutor::Submit(
    const std::string& host_name, monitor::proto::MonitorInfo info) {
  if (host_name.empty()) {
    return DataReceiveResult::kInvalidHost;
  }
  if (!accepting_.load(std::memory_order_acquire)) {
    return DataReceiveResult::kStopping;
  }

  const std::size_t shard_id = ShardFor(host_name);
  const auto received_at = std::chrono::system_clock::now();
  const auto enqueued_at = std::chrono::steady_clock::now();
  if (shards_[shard_id]->queue.TryPush(WorkItem{
          host_name, std::move(info), received_at, enqueued_at})) {
    return DataReceiveResult::kAccepted;
  }
  return accepting_.load(std::memory_order_acquire)
             ? DataReceiveResult::kQueueFull
             : DataReceiveResult::kStopping;
}

std::size_t HostShardExecutor::PeakQueueDepth() const {
  std::size_t peak = 0;
  for (const auto& shard : shards_) {
    peak = std::max(peak, shard->queue.PeakSize());
  }
  return peak;
}

std::size_t HostShardExecutor::PeakQueueBytes() const {
  std::size_t peak = 0;
  for (const auto& shard : shards_) {
    peak = std::max(peak, shard->queue.PeakBytes());
  }
  return peak;
}

std::size_t HostShardExecutor::ShardFor(const std::string& host_name) const {
  return std::hash<std::string>{}(host_name) % shards_.size();
}

void HostShardExecutor::RunShard(std::size_t shard_id) {
  WorkItem item;
  while (shards_[shard_id]->queue.Pop(&item)) {
    callback_(shard_id, item.host_name, item.info, item.received_at,
              item.enqueued_at);
  }
}

}  // namespace monitor
