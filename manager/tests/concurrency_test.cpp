#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "concurrency/bounded_queue.h"
#include "host_shard_executor.h"
#include "persistence_worker.h"

namespace {

constexpr std::size_t kLargeQueueBytes = 1024 * 1024;

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

void TestSameHostOrdering() {
  std::vector<int> processed;
  monitor::HostShardExecutor executor(
      4, 10000, kLargeQueueBytes,
      [&processed](std::size_t, const std::string&,
                   const monitor::proto::MonitorInfo& info,
                   std::chrono::system_clock::time_point,
                   std::chrono::steady_clock::time_point) {
        processed.push_back(std::stoi(info.name()));
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
          return;
        }
        {
          std::lock_guard<std::mutex> lock(mutex);
          received_at = item_received_at;
          processed_at = std::chrono::steady_clock::now();
        }
        condition.notify_all();
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

void TestHashCollisionPreservesEachHostOrder() {
  auto executor = std::make_unique<monitor::HostShardExecutor>(
      4, 128, kLargeQueueBytes,
      [](std::size_t, const std::string&, const monitor::proto::MonitorInfo&,
         std::chrono::system_clock::time_point,
         std::chrono::steady_clock::time_point) {});
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
  assert(executor.Submit("host", MakeInfo(3)) ==
         monitor::DataReceiveResult::kQueueFull);
  {
    std::lock_guard<std::mutex> lock(mutex);
    release_first = true;
  }
  condition.notify_all();
  executor.Stop();
  assert(processed == 2);
  assert(executor.Submit("host", MakeInfo(4)) ==
         monitor::DataReceiveResult::kStopping);
}

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
}

void TestPersistenceWorkerDrainsAcceptedTasks() {
  std::mutex mutex;
  int processed = 0;
  monitor::PersistenceWorker worker(
      2, kLargeQueueBytes,
      [&mutex, &processed](monitor::PersistenceTask&&) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        std::lock_guard<std::mutex> lock(mutex);
        ++processed;
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

}  // namespace

int main() {
  TestSameHostOrdering();
  TestDifferentHostsRunConcurrently();
  TestQueuedTimestampPreserved();
  TestHashCollisionPreservesEachHostOrder();
  TestQueueFullAndShutdownDrain();
  TestQueueByteBudgetAndOversize();
  TestPersistenceWorkerDrainsAcceptedTasks();
  TestPersistenceProducerUnblocksDuringDrain();
  return 0;
}
