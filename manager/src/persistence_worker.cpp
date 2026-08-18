#include "persistence_worker.h"

#include <utility>

namespace monitor {

PersistenceWorker::PersistenceWorker(std::size_t queue_capacity,
                                     TaskHandler handler)
    : queue_(queue_capacity), handler_(std::move(handler)) {}

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

void PersistenceWorker::Run() {
  PersistenceTask task;
  while (queue_.Pop(&task)) {
    handler_(std::move(task));
    task = PersistenceTask{};
  }
}

}  // namespace monitor
