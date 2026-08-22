#pragma once

/**
 * @file persistence_worker.h
 * @brief Manager 异步 PersistenceTask 消费线程接口。
 *
 * 入队线程只执行有界 TryPush；Run() 在线程中调用 TaskHandler，通常进入
 * HostManager::PersistTask 执行 MySQL。关闭队列后 worker 消费完可见任务再
 * join，避免 RPC 接收线程同步等待数据库。
 */

#include <atomic>
#include <cstddef>
#include <functional>
#include <thread>

#include "concurrency/bounded_queue.h"
#include "persistence_task.h"

namespace monitor {

/** @brief 以有界字节队列和单 worker 线程执行持久化任务。 */
class PersistenceWorker {
 public:
  using TaskHandler = std::function<void(PersistenceTask&&)>;

  PersistenceWorker(std::size_t queue_capacity, std::size_t queue_max_bytes,
                    TaskHandler handler);
  ~PersistenceWorker();

  PersistenceWorker(const PersistenceWorker&) = delete;
  PersistenceWorker& operator=(const PersistenceWorker&) = delete;

  /** @brief 启动唯一持久化消费线程。 */
  void Start();
  /** @brief 尝试将任务加入有界队列，未启动/满队列时返回 false。 */
  bool Enqueue(PersistenceTask task);
  /** @brief 关闭队列并等待持久化线程退出。 */
  void Stop();
  std::size_t PeakQueueDepth() const;
  std::size_t PeakQueueBytes() const;

 private:
  /** @brief 消费队列并调用外部 TaskHandler。 */
  void Run();

  concurrency::BoundedQueue<PersistenceTask> queue_;
  const TaskHandler handler_;
  std::thread worker_;
  std::atomic<bool> started_{false};
};

}  // namespace monitor
