#pragma once

#include <atomic>
#include <cstddef>
#include <functional>
#include <thread>

#include "concurrency/bounded_queue.h"
#include "persistence_task.h"

namespace monitor {

class PersistenceWorker {
 public:
  using TaskHandler = std::function<void(PersistenceTask&&)>;

  PersistenceWorker(std::size_t queue_capacity, std::size_t queue_max_bytes,
                    TaskHandler handler);
  ~PersistenceWorker();

  PersistenceWorker(const PersistenceWorker&) = delete;
  PersistenceWorker& operator=(const PersistenceWorker&) = delete;

  void Start();
  bool Enqueue(PersistenceTask task);
  void Stop();
  std::size_t PeakQueueDepth() const;
  std::size_t PeakQueueBytes() const;

 private:
  void Run();

  concurrency::BoundedQueue<PersistenceTask> queue_;
  const TaskHandler handler_;
  std::thread worker_;
  std::atomic<bool> started_{false};
};

}  // namespace monitor
