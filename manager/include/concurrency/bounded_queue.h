#pragma once

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace monitor::concurrency {

template <typename T>
class BoundedQueue {
 public:
  using WeightFn = std::function<std::size_t(const T&)>;

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
