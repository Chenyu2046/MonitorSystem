#include "persistence_worker.h"

#include <utility>

namespace monitor {

PersistenceWorker::PersistenceWorker(std::size_t queue_capacity,
                                     std::size_t queue_max_bytes,
                                     TaskHandler handler)
    : queue_(queue_capacity, queue_max_bytes,
             [](const PersistenceTask& task) {
               return EstimatePersistenceTaskBytes(task);
             }),
      handler_(std::move(handler)) {}

PersistenceWorker::~PersistenceWorker() { Stop(); }

void PersistenceWorker::Start() {
  bool expected = false;
  if (!started_.compare_exchange_strong(expected, true)) {
    return;
  }
  worker_ = std::thread(&PersistenceWorker::Run, this);
}

bool PersistenceWorker::Enqueue(PersistenceTask task) {
  if (!started_.load(std::memory_order_acquire)) {
    return false;
  }
  return queue_.Push(std::move(task));
}

void PersistenceWorker::Stop() {
  bool expected = true;
  if (!started_.compare_exchange_strong(expected, false)) {
    return;
  }
  queue_.Close();
  if (worker_.joinable()) {
    worker_.join();
  }
}

std::size_t PersistenceWorker::PeakQueueDepth() const {
  return queue_.PeakSize();
}

std::size_t PersistenceWorker::PeakQueueBytes() const {
  return queue_.PeakBytes();
}

void PersistenceWorker::Run() {
  PersistenceTask task;
  while (queue_.Pop(&task)) {
    handler_(std::move(task));
    task = PersistenceTask{};
  }
}

}  // namespace monitor
