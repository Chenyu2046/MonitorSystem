/**
 * @file monitor_send_queue.cpp
 * @brief 有界 MonitorInfo 队列的容量、优先级和线程同步实现。
 *
 * Push 在 mutex_ 内维护 item/byte 双重容量，DropOldestBaseMetric() 优先
 * 清理非诊断样本；Pop 在 condition_variable 上等待，Close() 用于让
 * sender thread 在没有剩余数据时退出。
 */

#include "rpc/monitor_send_queue.h"

#include <algorithm>
#include <utility>

namespace monitor {

MonitorSendQueue::MonitorSendQueue(std::size_t max_items, std::size_t max_bytes)
    : max_items_(std::max<std::size_t>(1, max_items)),
      max_bytes_(std::max<std::size_t>(1, max_bytes)) {}

void MonitorSendQueue::Open() {
  // Open 与发送线程启动配对；队列初始为 closed，避免 Start() 之前误入队。
  std::lock_guard<std::mutex> lock(mutex_);
  closed_ = false;
}

bool MonitorSendQueue::Push(monitor::proto::MonitorInfo info) {
  // ByteSizeLong() 以序列化大小参与容量控制，防止 profiling stack 让
  // 消息条数未超限但内存占用突破 max_bytes_。
  const std::size_t bytes = info.ByteSizeLong();
  const bool priority = IsPriority(info);
  std::lock_guard<std::mutex> lock(mutex_);
  if (closed_ || bytes > max_bytes_) {
    ++dropped_count_;
    return false;
  }

  // 先丢最旧普通指标；只有 priority 消息仍无法容纳时才丢队头，尽量
  // 保留诊断证据而不改变队列 FIFO 的基本发送顺序。
  while (!HasCapacity(bytes)) {
    if (DropOldestBaseMetric()) {
      continue;
    }

    if (!priority || items_.empty()) {
      ++dropped_count_;
      return false;
    }

    bytes_ -= items_.front().bytes;
    items_.pop_front();
    ++dropped_count_;
  }

  bytes_ += bytes;
  items_.push_back(Item{std::move(info), bytes, priority});
  condition_.notify_one();
  return true;
}

bool MonitorSendQueue::Pop(monitor::proto::MonitorInfo* info) {
  // 关闭只阻止继续等待；若队列中仍有消息，先把剩余消息发送完。
  std::unique_lock<std::mutex> lock(mutex_);
  condition_.wait(lock, [this] { return closed_ || !items_.empty(); });
  if (items_.empty()) {
    return false;
  }

  Item item = std::move(items_.front());
  items_.pop_front();
  bytes_ -= item.bytes;
  *info = std::move(item.info);
  return true;
}

void MonitorSendQueue::Close() {
  // notify_all 让等待中的 Pop() 重新检查 closed_，避免 Stop() 等待线程
  // 时永久阻塞。
  {
    std::lock_guard<std::mutex> lock(mutex_);
    closed_ = true;
  }
  condition_.notify_all();
}

std::size_t MonitorSendQueue::size() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return items_.size();
}

std::uint64_t MonitorSendQueue::dropped_count() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return dropped_count_;
}

bool MonitorSendQueue::IsPriority(const monitor::proto::MonitorInfo& info) {
  // 异常状态、较高分数或 profiling 结果都视为诊断证据，拥有比普通
  // 基础指标更高的保留优先级。
  if (!info.has_diagnostic()) {
    return false;
  }
  const auto& diagnostic = info.diagnostic();
  return diagnostic.state() != monitor::proto::OBSERVABILITY_NORMAL ||
         diagnostic.anomaly_score() >= 0.6 ||
         diagnostic.oncpu_profiles_size() > 0 ||
         diagnostic.offcpu_profiles_size() > 0;
}

bool MonitorSendQueue::HasCapacity(std::size_t bytes) const {
  return items_.size() < max_items_ && bytes <= max_bytes_ - bytes_;
}

bool MonitorSendQueue::DropOldestBaseMetric() {
  // 在保持优先级消息的前提下删除最早的普通样本；返回 false 表示队列
  // 已全部由 priority 项组成。
  const auto item =
      std::find_if(items_.begin(), items_.end(),
                   [](const Item& queued) { return !queued.priority; });
  if (item == items_.end()) {
    return false;
  }
  bytes_ -= item->bytes;
  items_.erase(item);
  ++dropped_count_;
  return true;
}

}  // namespace monitor
