/**
 * @file persistence_worker.cpp
 * @brief PersistenceTask 有界队列和后台消费线程实现。
 */

#include "persistence_worker.h"

#include <utility>

namespace monitor {

PersistenceWorker::PersistenceWorker(std::size_t queue_capacity,
                                     std::size_t queue_max_bytes,
                                     TaskHandler handler)
    // 队列大小估算包含 protobuf/detail 字符串，避免只按对象 sizeof
    // 限制而低估实际内存占用。
    : queue_(queue_capacity, queue_max_bytes,
             [](const PersistenceTask& task) {
               return EstimatePersistenceTaskBytes(task);
             }),
      handler_(std::move(handler)) {}

PersistenceWorker::~PersistenceWorker() { Stop(); }

void PersistenceWorker::Start() {
  // started_ 保证重复 Start 不会创建多个消费者；单消费者保持 SQL 写入
  // 顺序，并把数据库阻塞隔离在独立线程。
  bool expected = false;
  if (!started_.compare_exchange_strong(expected, true)) {
    return;
  }
  worker_ = std::thread(&PersistenceWorker::Run, this);
}

bool PersistenceWorker::Enqueue(PersistenceTask task) {
  // 接收路径只在 worker 已启动时入队；BoundedQueue 负责数量/字节背压。
  if (!started_.load(std::memory_order_acquire)) {
    return false;
  }
  return queue_.Push(std::move(task));
}

void PersistenceWorker::Stop() {
  // 关闭队列唤醒 Run()，随后 join 保证析构前 handler 不再访问 this。
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
  // Pop 返回 false 表示队列已关闭且没有剩余任务；每轮清空 task，避免
  // 上一条 protobuf/detail 数据在下一轮复用时造成额外保留。
  PersistenceTask task;
  while (queue_.Pop(&task)) {
    (void)handler_(std::move(task));
    task = PersistenceTask{};
  }
}

}  // namespace monitor
