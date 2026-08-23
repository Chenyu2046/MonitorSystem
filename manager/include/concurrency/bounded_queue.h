#pragma once

/**
 * @file bounded_queue.h
 * @brief 同时按元素数量和估算字节数限制的线程安全 FIFO 队列。
 *
 * TryPush 用于不阻塞的 RPC/接收路径，Push 可等待容量，Pop 在数据或关闭
 * 时唤醒。所有容量、peak 和 closed 状态由 mutex_ 保护；Close() 会同时
 * 唤醒 not_empty_ 与 not_full_，为线程生命周期提供明确退出边界。
 */

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace monitor::concurrency {

/**
 * @brief 为任意任务类型提供有界、可关闭的 FIFO 队列。
 *
 * WeightFn 允许调用方把 protobuf/string 等动态内存计入容量，避免只用
 * sizeof(T) 低估跨线程缓存占用。
 */
template <typename T>
class BoundedQueue {
 public:
  using WeightFn = std::function<std::size_t(const T&)>;
  enum class PopResult { kItem, kTimeout, kClosed };

  BoundedQueue(std::size_t max_items, std::size_t max_bytes)
      : BoundedQueue(max_items, max_bytes,
                     [](const T&) { return sizeof(T); }) {}

  BoundedQueue(std::size_t max_items, std::size_t max_bytes,
               WeightFn weight_fn)
      : max_items_(max_items),
        max_bytes_(max_bytes),
        weight_fn_(std::move(weight_fn)) {
    if (max_items_ == 0) {
      throw std::invalid_argument("BoundedQueue max_items must be positive");
    }
    if (max_bytes_ == 0) {
      throw std::invalid_argument("BoundedQueue max_bytes must be positive");
    }
    if (!weight_fn_) {
      throw std::invalid_argument("BoundedQueue weight function is required");
    }
  }

  BoundedQueue(const BoundedQueue&) = delete;
  BoundedQueue& operator=(const BoundedQueue&) = delete;

  /** @brief 不等待容量，满/关闭/单项超限时返回 false。 */
  bool TryPush(T value) {
    const std::size_t weight = weight_fn_(value);
    if (weight > max_bytes_) {
      return false;
    }
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (closed_ || !HasCapacity(weight)) {
        return false;
      }
      queue_.push_back(Entry{std::move(value), weight});
      current_bytes_ += weight;
      UpdatePeaks();
    }
    not_empty_.notify_one();
    return true;
  }

  /** @brief 等待容量或关闭后入队；关闭时返回 false。 */
  bool Push(T value) {
    const std::size_t weight = weight_fn_(value);
    if (weight > max_bytes_) {
      return false;
    }
    {
      std::unique_lock<std::mutex> lock(mutex_);
      not_full_.wait(lock, [this, weight] {
        return closed_ || HasCapacity(weight);
      });
      if (closed_) {
        return false;
      }
      queue_.push_back(Entry{std::move(value), weight});
      current_bytes_ += weight;
      UpdatePeaks();
    }
    not_empty_.notify_one();
    return true;
  }

  /** @brief 等待数据或关闭，并移动出队头元素。 */
  bool Pop(T* value) {
    if (!value) {
      return false;
    }
    {
      std::unique_lock<std::mutex> lock(mutex_);
      not_empty_.wait(lock,
                      [this] { return closed_ || !queue_.empty(); });
      if (queue_.empty()) {
        return false;
      }
      Entry& entry = queue_.front();
      *value = std::move(entry.value);
      current_bytes_ -= entry.weight;
      queue_.pop_front();
    }
    not_full_.notify_all();
    return true;
  }

  /** @brief 限时等待元素；超时与关闭分开返回，供 shard 定时维护。 */
  template <typename Rep, typename Period>
  PopResult PopFor(T* value,
                   const std::chrono::duration<Rep, Period>& timeout) {
    if (!value) return PopResult::kClosed;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      if (!not_empty_.wait_for(
              lock, timeout,
              [this] { return closed_ || !queue_.empty(); })) {
        return PopResult::kTimeout;
      }
      if (queue_.empty()) return PopResult::kClosed;
      Entry& entry = queue_.front();
      *value = std::move(entry.value);
      current_bytes_ -= entry.weight;
      queue_.pop_front();
    }
    not_full_.notify_all();
    return PopResult::kItem;
  }

  /** @brief 关闭队列并唤醒所有生产者/消费者。 */
  void Close() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      closed_ = true;
    }
    not_empty_.notify_all();
    not_full_.notify_all();
  }

  std::size_t Size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return queue_.size();
  }

  std::size_t Bytes() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return current_bytes_;
  }

  std::size_t PeakSize() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return peak_size_;
  }

  std::size_t PeakBytes() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return peak_bytes_;
  }

  std::size_t MaxItems() const { return max_items_; }
  std::size_t MaxBytes() const { return max_bytes_; }

 private:
  /** @brief 队列值及其动态占用估算。 */
  struct Entry {
    T value;
    std::size_t weight;
  };

  bool HasCapacity(std::size_t weight) const {
    return queue_.size() < max_items_ &&
           weight <= max_bytes_ - current_bytes_;
  }

  void UpdatePeaks() {
    if (queue_.size() > peak_size_) {
      peak_size_ = queue_.size();
    }
    if (current_bytes_ > peak_bytes_) {
      peak_bytes_ = current_bytes_;
    }
  }

  const std::size_t max_items_;
  const std::size_t max_bytes_;
  const WeightFn weight_fn_;
  mutable std::mutex mutex_;
  std::condition_variable not_empty_;
  std::condition_variable not_full_;
  std::deque<Entry> queue_;
  std::size_t current_bytes_ = 0;
  std::size_t peak_size_ = 0;
  std::size_t peak_bytes_ = 0;
  bool closed_ = false;
};

}  // namespace monitor::concurrency
