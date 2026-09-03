#pragma once

#include <chrono>
#include <cstddef>
#include <deque>
#include <utility>

namespace monitor::health {

class RollingWindow {
 public:
  // Health windows use Worker event time. Manager activity time is tracked
  // separately by HealthScoreEngine for stale eviction.
  using Clock = std::chrono::system_clock;

  // 创建受样本数和时间跨度双重限制的事件时间窗口。
  RollingWindow(std::size_t max_samples, std::chrono::seconds max_age);

  // 追加单调递增时间戳的有限样本；成功返回 true。
  bool Push(double value, Clock::time_point timestamp);
  // 删除相对给定事件时间已经过期的样本。
  void Prune(Clock::time_point timestamp);
  // 返回当前窗口中的样本数。
  std::size_t Size() const { return samples_.size(); }
  // 返回最早样本距给定事件时间的跨度。
  std::chrono::seconds Age(Clock::time_point timestamp) const;
  // 返回窗口样本的中位数。
  double Median() const;
  // 返回窗口样本的中位绝对偏差。
  double Mad() const;
  // 返回指数加权均值和标准差。
  std::pair<double, double> EwmaMeanStd(double alpha) const;

 private:
  struct Sample {
    double value;
    Clock::time_point timestamp;
  };

  std::size_t max_samples_;
  std::chrono::seconds max_age_;
  std::deque<Sample> samples_;
};

}  // namespace monitor::health
