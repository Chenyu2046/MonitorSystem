#pragma once

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace monitor::concurrency {

template <typename T>
class BoundedQueue {
 public:
  explicit BoundedQueue(std::size_t capacity) : capacity_(capacity) {
    if (capacity_ == 0) {
      throw std::invalid_argument("BoundedQueue capacity must be positive");
    }
  }

  BoundedQueue(const BoundedQueue&) = delete;
  BoundedQueue& operator=(const BoundedQueue&) = delete;

  bool TryPush(T value) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (closed_ || queue_.size() >= capacity_) {
        return false;
      }
      queue_.push_back(std::move(value));
    }
    not_empty_.notify_one();
    return true;
  }

  bool Push(T value) {
    {
      std::unique_lock<std::mutex> lock(mutex_);
      not_full_.wait(lock, [this] {
        return closed_ || queue_.size() < capacity_;
      });
      if (closed_) {
        return false;
      }
      queue_.push_back(std::move(value));
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
      *value = std::move(queue_.front());
      queue_.pop_front();
    }
    not_full_.notify_one();
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

 private:
  const std::size_t capacity_;
  mutable std::mutex mutex_;
  std::condition_variable not_empty_;
  std::condition_variable not_full_;
  std::deque<T> queue_;
  bool closed_ = false;
};

}  // namespace monitor::concurrency
