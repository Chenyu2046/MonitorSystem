#pragma once

/**
 * @file host_shard_executor.h
 * @brief 按 host 哈希分片的有界并发执行器。
 *
 * Submit 将 MonitorInfo 放入固定 shard 队列；同一 host 始终由同一个
 * worker 顺序处理，因此前后快照/变化率安全，而不同 host 可并行。队列
 * 同时受数量和序列化字节上限约束，满时返回 kQueueFull。
 */

#include <atomic>
#include <chrono>
#include <cstddef>
#include <functional>
#include <future>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "concurrency/bounded_queue.h"
#include "data_receive_result.h"
#include "monitor_info.pb.h"

namespace monitor {

/** @brief 维护 host -> shard 亲和性并运行每个 shard 的消费线程。 */
class HostShardExecutor {
 public:
  using ProcessCallback = std::function<HostFeedbackResult(
      std::size_t shard_id, const std::string& host_name,
      const monitor::proto::MonitorInfo& info,
      std::chrono::system_clock::time_point received_at,
      std::chrono::steady_clock::time_point enqueued_at)>;
  using MaintenanceCallback = std::function<void(
      std::size_t shard_id, std::chrono::steady_clock::time_point now)>;

  HostShardExecutor(std::size_t shard_count, std::size_t queue_capacity,
                    std::size_t queue_max_bytes, ProcessCallback callback,
                    MaintenanceCallback maintenance_callback = {},
                    std::chrono::milliseconds maintenance_interval =
                        std::chrono::minutes(1));
  ~HostShardExecutor();

  HostShardExecutor(const HostShardExecutor&) = delete;
  HostShardExecutor& operator=(const HostShardExecutor&) = delete;

  void Start();
  void Stop();

  DataReceiveResult Submit(
      const std::string& host_name, monitor::proto::MonitorInfo info);
  DataReceiveResult SubmitTracked(
      const std::string& host_name, monitor::proto::MonitorInfo info,
      std::shared_ptr<std::promise<HostFeedbackResult>> completion);

  std::size_t ShardFor(const std::string& host_name) const;
  std::size_t shard_count() const { return shards_.size(); }
  std::size_t PeakQueueDepth() const;
  std::size_t PeakQueueBytes() const;

 private:
  /** @brief shard 队列元素，携带接收时间和入队时间用于延迟观测。 */
  struct WorkItem {
    std::string host_name;
    monitor::proto::MonitorInfo info;
    std::chrono::system_clock::time_point received_at;
    std::chrono::steady_clock::time_point enqueued_at;
    std::shared_ptr<std::promise<HostFeedbackResult>> completion;
  };

  /** @brief 一个有界 MonitorInfo 队列及其唯一消费线程。 */
  struct Shard {
    Shard(std::size_t capacity, std::size_t max_bytes)
        : queue(capacity, max_bytes, [](const WorkItem& item) {
            return sizeof(WorkItem) + item.host_name.size() +
                   item.info.ByteSizeLong();
          }) {}

    concurrency::BoundedQueue<WorkItem> queue;
    std::thread worker;
  };

  /** @brief 在指定 shard 上按 FIFO 取任务并调用 ProcessCallback。 */
  void RunShard(std::size_t shard_id);
  DataReceiveResult SubmitImpl(
      const std::string& host_name, monitor::proto::MonitorInfo info,
      std::shared_ptr<std::promise<HostFeedbackResult>> completion);

  const ProcessCallback callback_;
  const MaintenanceCallback maintenance_callback_;
  const std::chrono::milliseconds maintenance_interval_;
  std::vector<std::unique_ptr<Shard>> shards_;
  std::atomic<bool> accepting_{false};
  bool started_ = false;
};

}  // namespace monitor
