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

  RollingWindow(std::size_t max_samples, std::chrono::seconds max_age);

  bool Push(double value, Clock::time_point timestamp);
  void Prune(Clock::time_point timestamp);
  std::size_t Size() const { return samples_.size(); }
  std::chrono::seconds Age(Clock::time_point timestamp) const;
  double Median() const;
  double Mad() const;
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
