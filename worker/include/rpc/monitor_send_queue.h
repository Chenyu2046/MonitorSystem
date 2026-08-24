#pragma once

/**
 * @file monitor_send_queue.h
 * @brief Worker 采集线程与 gRPC 发送线程之间的有界优先级队列。
 *
 * 队列同时受 item 数量和序列化字节数限制；诊断/异常消息标记为 priority，
 * 空间不足时优先丢弃普通基础监控，避免诊断证据被普通样本淹没。所有
 * 队列状态由 mutex_ 保护，Pop() 用 condition_variable 等待数据或关闭。
 */

#include <condition_variable>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>

#include "monitor_info.pb.h"

namespace monitor {

/** @brief Worker queue item with local enqueue timing; never serialized. */
struct PendingMonitorSample {
  monitor::proto::MonitorInfo info;
  std::chrono::steady_clock::time_point enqueued_at;
};

/**
 * @brief 在线程边界上传递 MonitorInfo，并实施背压和丢弃策略。
 */
class MonitorSendQueue {
 public:
  MonitorSendQueue(std::size_t max_items, std::size_t max_bytes);

  /** @brief 打开队列，允许采集线程入队。 */
  void Open();
  /** @brief 按大小/优先级入队，容量不足时按策略丢弃并计数。 */
  bool Push(monitor::proto::MonitorInfo info);
  bool Push(monitor::proto::MonitorInfo info,
            std::chrono::steady_clock::time_point enqueued_at);
  /** @brief 阻塞到有数据或关闭，并移动出一条消息。 */
  bool Pop(monitor::proto::MonitorInfo* info);
  bool Pop(PendingMonitorSample* sample);
  /** @brief 标记关闭并唤醒所有等待中的发送线程。 */
  void Close();

  std::size_t size() const;
  std::size_t bytes() const;
  std::uint64_t dropped_count() const;

 private:
  /** @brief 队列项及其序列化大小、诊断优先级标记。 */
  struct Item {
    monitor::proto::MonitorInfo info;
    std::size_t bytes = 0;
    bool priority = false;
    std::chrono::steady_clock::time_point enqueued_at;
  };

  static bool IsPriority(const monitor::proto::MonitorInfo& info);
  bool HasCapacity(std::size_t bytes) const;
  bool DropOldestBaseMetric();

  const std::size_t max_items_;
  const std::size_t max_bytes_;
  mutable std::mutex mutex_;
  std::condition_variable condition_;
  std::deque<Item> items_;
  std::size_t bytes_ = 0;
  std::uint64_t dropped_count_ = 0;
  bool closed_ = true;
};

}  // namespace monitor
