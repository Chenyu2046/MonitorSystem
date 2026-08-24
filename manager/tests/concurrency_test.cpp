/**
 * @file concurrency_test.cpp
 * @brief 验证 host shard 顺序/并发、有界队列和 persistence worker 生命周期。
 *
 * 测试关注线程模型契约：同一 host 顺序、不同 host 并行、队列满/关闭
 * 行为、字节预算和已接受任务 drain，防止并发改造破坏变化率前后基线。
 */

#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "concurrency/bounded_queue.h"
#include "host_shard_executor.h"
#include "persistence_worker.h"

namespace {

constexpr std::size_t kLargeQueueBytes = 1024 * 1024;

/** @brief 构造带 sequence 的最小队列测试消息。 */
monitor::proto::MonitorInfo MakeInfo(int sequence) {
  monitor::proto::MonitorInfo info;
  info.set_name(std::to_string(sequence));
  return info;
}

void UpdateMax(std::atomic<int>* maximum, int value) {
  int current = maximum->load(std::memory_order_relaxed);
  while (current < value &&
         !maximum->compare_exchange_weak(current, value,
                                         std::memory_order_relaxed,
                                         std::memory_order_relaxed)) {
  }
}

/** @brief 验证同一 host 在单 shard 内保持 FIFO 顺序。 */
void TestSameHostOrdering() {
  std::vector<int> processed;
  monitor::HostShardExecutor executor(
      4, 10000, kLargeQueueBytes,
      [&processed](std::size_t, const std::string&,
                   const monitor::proto::MonitorInfo& info,
                   std::chrono::system_clock::time_point,
                   std::chrono::steady_clock::time_point) {
        processed.push_back(std::stoi(info.name()));
        return monitor::HostFeedbackResult{};
      });
  executor.Start();
  for (int sequence = 0; sequence < 10000; ++sequence) {
    assert(executor.Submit("host-a", MakeInfo(sequence)) ==
           monitor::DataReceiveResult::kAccepted);
  }
  executor.Stop();

  assert(processed.size() == 10000);
  for (int sequence = 0; sequence < 10000; ++sequence) {
    assert(processed[sequence] == sequence);
  }
}

/** @brief 验证不同 host 可由不同 shard 并行处理。 */
void TestDifferentHostsRunConcurrently() {
  std::atomic<int> active{0};
  std::atomic<int> max_active{0};
  std::mutex mutex;
  std::condition_variable condition;
  bool release = false;
  monitor::HostShardExecutor executor(
      4, 16, kLargeQueueBytes,
      [&](std::size_t, const std::string&, const monitor::proto::MonitorInfo&,
          std::chrono::system_clock::time_point,
          std::chrono::steady_clock::time_point) {
        const int current = active.fetch_add(1) + 1;
        UpdateMax(&max_active, current);
        condition.notify_all();
        std::unique_lock<std::mutex> lock(mutex);
        condition.wait(lock, [&] { return release; });
        active.fetch_sub(1);
        return monitor::HostFeedbackResult{};
      });

  std::vector<std::string> hosts;
  std::map<std::size_t, std::string> by_shard;
  for (int index = 0; hosts.size() < 4 && index < 10000; ++index) {
    const std::string host = "host-" + std::to_string(index);
    const std::size_t shard = executor.ShardFor(host);
    if (by_shard.emplace(shard, host).second) {
      hosts.push_back(host);
    }
  }
  assert(hosts.size() == 4);

  executor.Start();
  for (const auto& host : hosts) {
    assert(executor.Submit(host, MakeInfo(1)) ==
           monitor::DataReceiveResult::kAccepted);
  }
  {
    std::unique_lock<std::mutex> lock(mutex);
    condition.wait_for(lock, std::chrono::seconds(2),
                       [&] { return max_active.load() >= 2; });
    release = true;
  }
  condition.notify_all();
  executor.Stop();
  assert(max_active.load() >= 2);
}

/** @brief 验证入队时间在 callback 中保留，用于排队延迟统计。 */
void TestQueuedTimestampPreserved() {
  std::mutex mutex;
  std::condition_variable condition;
  bool first_started = false;
  bool release_first = false;
  std::chrono::system_clock::time_point received_at;
  std::chrono::steady_clock::time_point processed_at;

  monitor::HostShardExecutor executor(
      1, 4, kLargeQueueBytes,
      [&](std::size_t, const std::string&,
          const monitor::proto::MonitorInfo& info,
          std::chrono::system_clock::time_point item_received_at,
          std::chrono::steady_clock::time_point) {
        if (info.name() == "1") {
          {
            std::lock_guard<std::mutex> lock(mutex);
            first_started = true;
          }
          condition.notify_all();
          std::unique_lock<std::mutex> lock(mutex);
          condition.wait(lock, [&] { return release_first; });
          return monitor::HostFeedbackResult{};
        }
        {
          std::lock_guard<std::mutex> lock(mutex);
          received_at = item_received_at;
          processed_at = std::chrono::steady_clock::now();
        }
        condition.notify_all();
        return monitor::HostFeedbackResult{};
      });

  executor.Start();
  assert(executor.Submit("host", MakeInfo(1)) ==
         monitor::DataReceiveResult::kAccepted);
  {
    std::unique_lock<std::mutex> lock(mutex);
    condition.wait(lock, [&] { return first_started; });
  }

  const auto submit_time = std::chrono::system_clock::now();
  assert(executor.Submit("host", MakeInfo(2)) ==
         monitor::DataReceiveResult::kAccepted);
  {
    std::lock_guard<std::mutex> lock(mutex);
    release_first = true;
  }
  condition.notify_all();
  executor.Stop();

  const auto delta = std::chrono::duration_cast<std::chrono::milliseconds>(
      received_at - submit_time);
  const auto absolute_delta = delta.count() < 0 ? -delta.count() : delta.count();
  assert(absolute_delta < 500);
  assert(processed_at.time_since_epoch().count() != 0);
}

/** @brief 验证 hash 碰撞时不同 host 仍各自保持消息顺序。 */
void TestHashCollisionPreservesEachHostOrder() {
  auto executor = std::make_unique<monitor::HostShardExecutor>(
      4, 128, kLargeQueueBytes,
      [](std::size_t, const std::string&, const monitor::proto::MonitorInfo&,
         std::chrono::system_clock::time_point,
         std::chrono::steady_clock::time_point) {
        return monitor::HostFeedbackResult{};
      });
  std::string first;
  std::string second;
  for (int index = 0; index < 10000 && second.empty(); ++index) {
    const std::string candidate = "collision-" + std::to_string(index);
    if (first.empty()) {
      first = candidate;
      continue;
    }
    if (executor->ShardFor(first) == executor->ShardFor(candidate)) {
      second = candidate;
    }
  }
  assert(!second.empty());

  std::map<std::string, std::vector<int>> processed;
  executor = std::make_unique<monitor::HostShardExecutor>(
      4, 128, kLargeQueueBytes,
      [&processed](std::size_t, const std::string& host,
                   const monitor::proto::MonitorInfo& info,
                   std::chrono::system_clock::time_point,
                   std::chrono::steady_clock::time_point) {
        processed[host].push_back(std::stoi(info.name()));
        return monitor::HostFeedbackResult{};
      });
  executor->Start();
  for (int sequence = 0; sequence < 50; ++sequence) {
    assert(executor->Submit(first, MakeInfo(sequence)) ==
           monitor::DataReceiveResult::kAccepted);
    assert(executor->Submit(second, MakeInfo(sequence)) ==
           monitor::DataReceiveResult::kAccepted);
  }
  executor->Stop();

  assert(processed[first].size() == 50);
  assert(processed[second].size() == 50);
  for (int sequence = 0; sequence < 50; ++sequence) {
    assert(processed[first][sequence] == sequence);
    assert(processed[second][sequence] == sequence);
  }
}

/** @brief 验证队列满返回 backpressure，关闭后已入队任务可 drain。 */
void TestQueueFullAndShutdownDrain() {
  std::mutex mutex;
  std::condition_variable condition;
  bool first_started = false;
  bool release_first = false;
  int processed = 0;
  monitor::HostShardExecutor executor(
      1, 1, kLargeQueueBytes,
      [&](std::size_t, const std::string&, const monitor::proto::MonitorInfo&,
          std::chrono::system_clock::time_point,
          std::chrono::steady_clock::time_point) {
        {
          std::lock_guard<std::mutex> lock(mutex);
          ++processed;
          first_started = true;
        }
        condition.notify_all();
        std::unique_lock<std::mutex> lock(mutex);
        condition.wait(lock, [&] { return release_first; });
        return monitor::HostFeedbackResult{};
      });
  executor.Start();
  assert(executor.Submit("host", MakeInfo(1)) ==
         monitor::DataReceiveResult::kAccepted);
  {
    std::unique_lock<std::mutex> lock(mutex);
    condition.wait(lock, [&] { return first_started; });
  }
  assert(executor.Submit("host", MakeInfo(2)) ==
         monitor::DataReceiveResult::kAccepted);
  auto rejected_info = MakeInfo(3);
  rejected_info.set_sample_session_id("session-B");
  rejected_info.set_sample_sequence(3);
  std::ostringstream captured;
  auto* old_buffer = std::cerr.rdbuf(captured.rdbuf());
  assert(executor.Submit("host", rejected_info) ==
         monitor::DataReceiveResult::kQueueFull);
  std::cerr.rdbuf(old_buffer);
  assert(captured.str().find("event=shard_queue_reject") !=
         std::string::npos);
  assert(captured.str().find("reason=queue_full") != std::string::npos);
  assert(captured.str().find("trace_id=host:session-B:3") !=
         std::string::npos);
  {
    std::lock_guard<std::mutex> lock(mutex);
    release_first = true;
  }
  condition.notify_all();
  executor.Stop();
  assert(processed == 2);
  assert(executor.Submit("host", MakeInfo(4)) ==
         monitor::DataReceiveResult::kStopping);
  assert(std::string(monitor::QueueRejectReason(
             monitor::DataReceiveResult::kQueueFull)) == "queue_full");
  assert(std::string(monitor::QueueRejectReason(
             monitor::DataReceiveResult::kStopping)) == "stopping");
}

/** @brief 验证队列已关闭时 reject 日志与 kStopping 返回值一致。 */
void TestStoppingQueueRejectReason() {
  std::mutex mutex;
  std::condition_variable condition;
  bool before_push = false;
  bool release_push = false;
  monitor::DataReceiveResult result = monitor::DataReceiveResult::kAccepted;
  monitor::HostShardExecutor executor(
      1, 1, kLargeQueueBytes,
      [](std::size_t, const std::string&,
         const monitor::proto::MonitorInfo&,
         std::chrono::system_clock::time_point,
         std::chrono::steady_clock::time_point) {
        return monitor::HostFeedbackResult{};
      },
      {}, std::chrono::minutes(1), [&] {
        std::unique_lock<std::mutex> lock(mutex);
        before_push = true;
        condition.notify_all();
        condition.wait(lock, [&] { return release_push; });
      });
  executor.Start();

  std::ostringstream captured;
  auto* old_buffer = std::cerr.rdbuf(captured.rdbuf());
  std::thread submitter([&] {
    auto info = MakeInfo(5);
    info.set_sample_session_id("session-stop");
    info.set_sample_sequence(5);
    result = executor.Submit("host", std::move(info));
  });
  {
    std::unique_lock<std::mutex> lock(mutex);
    assert(condition.wait_for(lock, std::chrono::seconds(2),
                              [&] { return before_push; }));
  }

  executor.Stop();
  {
    std::lock_guard<std::mutex> lock(mutex);
    release_push = true;
  }
  condition.notify_all();
  submitter.join();
  std::cerr.rdbuf(old_buffer);

  assert(result == monitor::DataReceiveResult::kStopping);
  assert(captured.str().find("reason=stopping") != std::string::npos);
  assert(captured.str().find("trace_id=host:session-stop:5") !=
         std::string::npos);
}

/** @brief 验证无消息时维护仍在 shard worker 执行，Stop 后不再回调。 */
void TestTimedMaintenanceUsesShardOwnerAndStops() {
  std::mutex mutex;
  std::condition_variable condition;
  std::thread::id process_thread;
  std::thread::id maintenance_thread;
  int maintenance_calls = 0;
  monitor::HostShardExecutor executor(
      1, 4, kLargeQueueBytes,
      [&](std::size_t, const std::string&,
          const monitor::proto::MonitorInfo&,
          std::chrono::system_clock::time_point,
          std::chrono::steady_clock::time_point) {
        std::lock_guard<std::mutex> lock(mutex);
        process_thread = std::this_thread::get_id();
        condition.notify_all();
        return monitor::HostFeedbackResult{};
      },
      [&](std::size_t, std::chrono::steady_clock::time_point) {
        std::lock_guard<std::mutex> lock(mutex);
        maintenance_thread = std::this_thread::get_id();
        ++maintenance_calls;
        condition.notify_all();
      },
      std::chrono::milliseconds(10));
  executor.Start();
  assert(executor.Submit("host", MakeInfo(1)) ==
         monitor::DataReceiveResult::kAccepted);
  {
    std::unique_lock<std::mutex> lock(mutex);
    assert(condition.wait_for(lock, std::chrono::seconds(2), [&] {
      return process_thread != std::thread::id{} && maintenance_calls > 0;
    }));
    assert(process_thread == maintenance_thread);
  }
  executor.Stop();
  int calls_after_stop = 0;
  {
    std::lock_guard<std::mutex> lock(mutex);
    calls_after_stop = maintenance_calls;
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(30));
  {
    std::lock_guard<std::mutex> lock(mutex);
    assert(maintenance_calls == calls_after_stop);
  }
}

/** @brief 验证队列持续非空时维护仍按绝对 deadline 插入执行。 */
void TestMaintenanceRunsWhileQueueRemainsNonEmpty() {
  std::atomic<int> processed{0};
  std::atomic<int> maintenance_calls{0};
  std::atomic<int> processed_at_first_maintenance{-1};
  monitor::HostShardExecutor executor(
      1, 256, kLargeQueueBytes,
      [&](std::size_t, const std::string&,
          const monitor::proto::MonitorInfo&,
          std::chrono::system_clock::time_point,
          std::chrono::steady_clock::time_point) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        processed.fetch_add(1, std::memory_order_relaxed);
        return monitor::HostFeedbackResult{};
      },
      [&](std::size_t, std::chrono::steady_clock::time_point) {
        int expected = -1;
        processed_at_first_maintenance.compare_exchange_strong(
            expected, processed.load(std::memory_order_relaxed));
        maintenance_calls.fetch_add(1, std::memory_order_relaxed);
      },
      std::chrono::milliseconds(10));
  executor.Start();
  constexpr int kItems = 100;
  for (int index = 0; index < kItems; ++index) {
    assert(executor.Submit("busy-host", MakeInfo(index)) ==
           monitor::DataReceiveResult::kAccepted);
  }
  executor.Stop();
  assert(processed.load() == kItems);
  assert(maintenance_calls.load() > 0);
  assert(processed_at_first_maintenance.load() > 0);
  assert(processed_at_first_maintenance.load() < kItems);
}

/** @brief 验证 tracked waiter 与 Stop 并发时 drain 且仅完成一次。 */
void TestTrackedWaiterCompletesDuringStopDrain() {
  std::mutex mutex;
  std::condition_variable condition;
  bool started = false;
  bool release = false;
  std::atomic<int> callbacks{0};
  monitor::HostShardExecutor executor(
      1, 4, kLargeQueueBytes,
      [&](std::size_t, const std::string& host,
          const monitor::proto::MonitorInfo&,
          std::chrono::system_clock::time_point,
          std::chrono::steady_clock::time_point) {
        callbacks.fetch_add(1, std::memory_order_relaxed);
        {
          std::lock_guard<std::mutex> lock(mutex);
          started = true;
        }
        condition.notify_all();
        std::unique_lock<std::mutex> lock(mutex);
        condition.wait(lock, [&] { return release; });
        monitor::HostFeedbackResult result;
        result.host_name = host;
        result.health_valid = true;
        result.result_version = 1;
        return result;
      });
  executor.Start();
  auto completion =
      std::make_shared<std::promise<monitor::HostFeedbackResult>>();
  auto completed = completion->get_future();
  assert(executor.SubmitTracked("tracked-host", MakeInfo(1), completion) ==
         monitor::DataReceiveResult::kAccepted);
  {
    std::unique_lock<std::mutex> lock(mutex);
    assert(condition.wait_for(lock, std::chrono::seconds(2),
                              [&] { return started; }));
  }
  std::thread stopper([&] { executor.Stop(); });
  {
    std::lock_guard<std::mutex> lock(mutex);
    release = true;
  }
  condition.notify_all();
  assert(completed.wait_for(std::chrono::seconds(2)) ==
         std::future_status::ready);
  const auto result = completed.get();
  assert(result.health_valid);
  assert(result.host_name == "tracked-host");
  stopper.join();
  assert(callbacks.load() == 1);
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  assert(callbacks.load() == 1);

  auto rejected =
      std::make_shared<std::promise<monitor::HostFeedbackResult>>();
  auto rejected_future = rejected->get_future();
  assert(executor.SubmitTracked("tracked-host", MakeInfo(2), rejected) ==
         monitor::DataReceiveResult::kStopping);
  assert(rejected_future.wait_for(std::chrono::milliseconds(10)) ==
         std::future_status::ready);
  assert(!rejected_future.get().health_valid);
}

/** @brief 验证字节预算和单项 oversize 拒绝语义。 */
void TestQueueByteBudgetAndOversize() {
  monitor::concurrency::BoundedQueue<int> queue(
      100, 1000, [](const int& value) { return static_cast<std::size_t>(value); });
  assert(queue.TryPush(600));
  assert(!queue.TryPush(500));
  assert(queue.Bytes() == 600);
  int value = 0;
  assert(queue.Pop(&value));
  assert(value == 600);
  assert(queue.TryPush(500));

  monitor::concurrency::BoundedQueue<std::string> small_queue(
      2, 4, [](const std::string& item) { return item.size(); });
  assert(!small_queue.TryPush("12345"));
  assert(!small_queue.Push("12345"));

  monitor::concurrency::BoundedQueue<std::string> rejected_input_queue(
      1, 5, [](const std::string& item) { return item.size(); });
  std::string rejected_input = "123456";
  assert(!rejected_input_queue.TryPush(std::move(rejected_input)));
  assert(rejected_input == "123456");
}

/** @brief 验证 PersistenceWorker 停止前消费已接受任务。 */
void TestPersistenceWorkerDrainsAcceptedTasks() {
  std::mutex mutex;
  int processed = 0;
  monitor::PersistenceWorker worker(
      2, kLargeQueueBytes,
      [&mutex, &processed](monitor::PersistenceTask&&) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        std::lock_guard<std::mutex> lock(mutex);
        ++processed;
        return true;
      });
  worker.Start();
  int accepted = 0;
  for (int index = 0; index < 100; ++index) {
    if (worker.Enqueue(monitor::PersistenceTask{})) {
      ++accepted;
    }
  }
  worker.Stop();
  assert(processed == accepted);
}

/** @brief 验证关闭/消费过程中被阻塞的生产者能恢复并退出。 */
void TestPersistenceProducerUnblocksDuringDrain() {
  std::mutex mutex;
  std::condition_variable condition;
  bool first_started = false;
  bool release_first = false;
  bool producer_started = false;
  bool producer_done = false;
  int processed = 0;
  monitor::PersistenceWorker worker(
      1, kLargeQueueBytes,
      [&](monitor::PersistenceTask&&) {
        {
          std::lock_guard<std::mutex> lock(mutex);
          ++processed;
          if (!first_started) {
            first_started = true;
            condition.notify_all();
          }
        }
        std::unique_lock<std::mutex> lock(mutex);
        if (processed == 1) {
          condition.wait(lock, [&] { return release_first; });
        }
        return true;
      });
  worker.Start();
  assert(worker.Enqueue(monitor::PersistenceTask{}));
  {
    std::unique_lock<std::mutex> lock(mutex);
    condition.wait(lock, [&] { return first_started; });
  }
  assert(worker.Enqueue(monitor::PersistenceTask{}));

  bool producer_result = false;
  std::thread producer([&] {
    {
      std::lock_guard<std::mutex> lock(mutex);
      producer_started = true;
    }
    condition.notify_all();
    producer_result = worker.Enqueue(monitor::PersistenceTask{});
    {
      std::lock_guard<std::mutex> lock(mutex);
      producer_done = true;
    }
    condition.notify_all();
  });
  {
    std::unique_lock<std::mutex> lock(mutex);
    condition.wait(lock, [&] { return producer_started; });
    assert(!producer_done);
    release_first = true;
  }
  condition.notify_all();
  producer.join();
  assert(producer_done);
  assert(producer_result);
  worker.Stop();
  assert(processed == 3);
}

/** @brief 验证持久化队列按业务任务计费且 handler 仍收到原任务。 */
void TestPersistenceQueueBudgetAndHandlerSemantics() {
  std::mutex mutex;
  std::condition_variable condition;
  bool first_started = false;
  bool release_first = false;
  int callbacks = 0;
  std::string received_host;

  monitor::PersistenceTask queued_task;
  queued_task.host_name = "queued-host";
  const std::size_t task_budget =
      monitor::EstimatePersistenceTaskBytes(queued_task);

  monitor::PersistenceWorker worker(
      2, task_budget,
      [&](monitor::PersistenceTask&& task) {
        std::unique_lock<std::mutex> lock(mutex);
        ++callbacks;
        if (callbacks == 1) {
          first_started = true;
          condition.notify_all();
          condition.wait(lock, [&] { return release_first; });
        } else {
          received_host = task.host_name;
        }
        return true;
      });
  worker.Start();

  monitor::PersistenceTask first_task;
  first_task.host_name = "a";
  assert(worker.Enqueue(std::move(first_task)));
  {
    std::unique_lock<std::mutex> lock(mutex);
    assert(condition.wait_for(lock, std::chrono::seconds(2),
                              [&] { return first_started; }));
  }

  assert(worker.Enqueue(queued_task));
  assert(worker.QueueDepth() == 1);
  assert(worker.QueueBytes() == task_budget);
  {
    std::lock_guard<std::mutex> lock(mutex);
    release_first = true;
  }
  condition.notify_all();
  worker.Stop();

  assert(callbacks == 2);
  assert(received_host == "queued-host");
}

/** @brief 验证持久化任务超过 byte 上限时不会进入 worker。 */
void TestPersistenceWorkerRejectsOversizeTask() {
  bool handler_called = false;
  monitor::PersistenceWorker worker(
      2, 1, [&handler_called](monitor::PersistenceTask&&) {
        handler_called = true;
        return true;
      });
  worker.Start();
  assert(!worker.Enqueue(monitor::PersistenceTask{}));
  worker.Stop();
  assert(!handler_called);
}

}  // namespace

int main() {
  setenv("MONITOR_PERF_LOG", "1", 1);
  unsetenv("MONITOR_PERF_TRACE");
  TestSameHostOrdering();
  TestDifferentHostsRunConcurrently();
  TestQueuedTimestampPreserved();
  TestHashCollisionPreservesEachHostOrder();
  TestQueueFullAndShutdownDrain();
  TestStoppingQueueRejectReason();
  TestTimedMaintenanceUsesShardOwnerAndStops();
  TestMaintenanceRunsWhileQueueRemainsNonEmpty();
  TestTrackedWaiterCompletesDuringStopDrain();
  TestQueueByteBudgetAndOversize();
  TestPersistenceWorkerDrainsAcceptedTasks();
  TestPersistenceProducerUnblocksDuringDrain();
  TestPersistenceQueueBudgetAndHandlerSemantics();
  TestPersistenceWorkerRejectsOversizeTask();
  unsetenv("MONITOR_PERF_LOG");
  return 0;
}
