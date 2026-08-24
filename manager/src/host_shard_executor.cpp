/**
 * @file host_shard_executor.cpp
 * @brief Host 哈希分片、队列背压和 worker 生命周期实现。
 *
 * 每个 shard 一个 BoundedQueue 和线程；hash(host_name)%shard_count 保证
 * 同一 host 的任务顺序。Stop() 先关闭所有队列，再 join 所有 worker，
 * 不在提交线程中执行实际业务处理。
 */

#include "host_shard_executor.h"

#include "perf/perf_log.h"

#include <algorithm>
#include <chrono>
#include <functional>
#include <utility>

namespace monitor {

HostShardExecutor::HostShardExecutor(std::size_t shard_count,
                                     std::size_t queue_capacity,
                                     std::size_t queue_max_bytes,
                                     ProcessCallback callback,
                                     MaintenanceCallback maintenance_callback,
                                     std::chrono::milliseconds
                                         maintenance_interval
#ifdef MONITOR_TESTING
                                     , BeforeQueuePushHook before_queue_push
#endif
                                     )
    : callback_(std::move(callback)),
      maintenance_callback_(std::move(maintenance_callback)),
      maintenance_interval_(maintenance_interval)
#ifdef MONITOR_TESTING
      , before_queue_push_(std::move(before_queue_push))
#endif
{
  // 至少保留一个 shard，避免合法配置为 0 时出现取模/无消费者。
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
  // accepting_ 在 worker 创建前打开；调用方应在 Start() 完成后再接受 RPC。
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
  // 先拒绝新任务，再关闭队列唤醒 Pop()，最后等待每个 shard 顺序消费
  // 完已有任务并退出。
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
  return SubmitImpl(host_name, std::move(info), nullptr);
}

DataReceiveResult HostShardExecutor::SubmitTracked(
    const std::string& host_name, monitor::proto::MonitorInfo info,
    std::shared_ptr<std::promise<HostFeedbackResult>> completion) {
  return SubmitImpl(host_name, std::move(info), std::move(completion));
}

DataReceiveResult HostShardExecutor::SubmitImpl(
    const std::string& host_name, monitor::proto::MonitorInfo info,
    std::shared_ptr<std::promise<HostFeedbackResult>> completion) {
  if (host_name.empty()) {
    if (completion) completion->set_value({});
    return DataReceiveResult::kInvalidHost;
  }
  if (!accepting_.load(std::memory_order_acquire)) {
    if (completion) completion->set_value({});
    return DataReceiveResult::kStopping;
  }

  // 时间戳分别使用 system_clock（事件时间）和 steady_clock（排队耗时），
  // 两者用途不同，不互相替代。
  const std::size_t shard_id = ShardFor(host_name);
  const auto received_at = std::chrono::system_clock::now();
  const auto enqueued_at = std::chrono::steady_clock::now();
  WorkItem item{host_name, std::move(info), received_at, enqueued_at,
                completion};
#ifdef MONITOR_TESTING
  if (before_queue_push_) before_queue_push_();
#endif
  if (shards_[shard_id]->queue.TryPush(std::move(item))) {
    return DataReceiveResult::kAccepted;
  }
  const auto reject_result =
      accepting_.load(std::memory_order_acquire)
          ? DataReceiveResult::kQueueFull
          : DataReceiveResult::kStopping;
  if (completion) completion->set_value({});
  if (perf::OutputEnabled()) {
    const auto trace_id = perf::BuildTraceId(host_name, item.info);
    perf::LogWarn("manager", "shard_queue_reject", trace_id, [&] {
      return "host=" + host_name + " shard=" + std::to_string(shard_id) +
             " queue_depth=" +
             std::to_string(shards_[shard_id]->queue.Size()) +
             " queue_bytes=" +
             std::to_string(shards_[shard_id]->queue.Bytes()) +
             " peak_depth=" +
             std::to_string(shards_[shard_id]->queue.PeakSize()) +
             " peak_bytes=" +
             std::to_string(shards_[shard_id]->queue.PeakBytes()) +
             " reason=" + QueueRejectReason(reject_result);
    });
  }
  return reject_result;
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
  // 稳定哈希提供 host 亲和性；顺序性只需在同一 shard 内成立。
  return std::hash<std::string>{}(host_name) % shards_.size();
}

void HostShardExecutor::RunShard(std::size_t shard_id) {
  // 一个 shard 只有一个消费者，因此 callback 看到同一 host 的消息是
  // FIFO 顺序；不同 shard 之间允许并行执行。
  WorkItem item;
  auto next_maintenance =
      std::chrono::steady_clock::now() + maintenance_interval_;
  while (true) {
    if (!maintenance_callback_) {
      if (!shards_[shard_id]->queue.Pop(&item)) return;
    } else {
      const auto now = std::chrono::steady_clock::now();
      const auto wait_for = now < next_maintenance
                                ? next_maintenance - now
                                : std::chrono::steady_clock::duration::zero();
      const auto result = shards_[shard_id]->queue.PopFor(&item, wait_for);
      if (result == decltype(shards_[shard_id]->queue)::PopResult::kTimeout) {
        const auto maintenance_now = std::chrono::steady_clock::now();
        maintenance_callback_(shard_id, maintenance_now);
        next_maintenance = maintenance_now + maintenance_interval_;
        continue;
      }
      if (result == decltype(shards_[shard_id]->queue)::PopResult::kClosed) {
        return;
      }
    }
    const auto queue_wait_us = perf::ElapsedUs(item.enqueued_at);
    const bool slow = perf::IsSlow(
        queue_wait_us, perf::GetConfig().slow_manager_queue_ms);
    if (perf::PerfTraceEnabled() || slow) {
      const auto trace_id = perf::BuildTraceId(item.host_name, item.info);
      const auto fields = [&] {
        return "shard=" + std::to_string(shard_id) +
               " queue_wait_us=" + std::to_string(queue_wait_us) +
               " queue_depth=" +
               std::to_string(shards_[shard_id]->queue.Size()) +
               " queue_bytes=" +
               std::to_string(shards_[shard_id]->queue.Bytes());
      };
      if (perf::PerfTraceEnabled()) {
        perf::LogPerf("manager", "shard_queue", trace_id, fields);
      } else {
        perf::LogSlow("manager", "shard_queue", trace_id, fields);
      }
    }
    const HostFeedbackResult result =
        callback_(shard_id, item.host_name, item.info, item.received_at,
                  item.enqueued_at);
    if (item.completion) item.completion->set_value(result);
    const auto now = std::chrono::steady_clock::now();
    if (maintenance_callback_ && now >= next_maintenance) {
      maintenance_callback_(shard_id, now);
      next_maintenance = now + maintenance_interval_;
    }
  }
}

}  // namespace monitor
