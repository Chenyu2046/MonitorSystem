#include <cassert>
#include <chrono>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "host_shard_executor.h"
#include "persistence_worker.h"

namespace {

monitor::proto::MonitorInfo MakeInfo(int sequence) {
  monitor::proto::MonitorInfo info;
  info.set_name(std::to_string(sequence));
  return info;
}

void TestSameHostOrdering() {
  std::vector<int> processed;
  monitor::HostShardExecutor executor(
      4, 10000,
      [&processed](std::size_t, const std::string&,
                   const monitor::proto::MonitorInfo& info) {
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
  monitor::HostShardExecutor executor(
      4, 16,
      [](std::size_t, const std::string&, const monitor::proto::MonitorInfo&) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
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
  const auto start = std::chrono::steady_clock::now();
  executor.Stop();
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - start);
  assert(elapsed.count() < 180);
}

void TestHashCollisionPreservesEachHostOrder() {
  auto executor = std::make_unique<monitor::HostShardExecutor>(
      4, 128,
      [](std::size_t, const std::string&, const monitor::proto::MonitorInfo&) {});
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
      4, 128,
      [&processed](std::size_t, const std::string& host,
                   const monitor::proto::MonitorInfo& info) {
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
      1, 1,
      [&](std::size_t, const std::string&, const monitor::proto::MonitorInfo&) {
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

void TestPersistenceWorkerDrainsAcceptedTasks() {
  std::mutex mutex;
  int processed = 0;
  monitor::PersistenceWorker worker(
      2, [&mutex, &processed](monitor::PersistenceTask&&) {
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

}  // namespace

int main() {
  TestSameHostOrdering();
  TestDifferentHostsRunConcurrently();
  TestHashCollisionPreservesEachHostOrder();
  TestQueueFullAndShutdownDrain();
  TestPersistenceWorkerDrainsAcceptedTasks();
  return 0;
}
