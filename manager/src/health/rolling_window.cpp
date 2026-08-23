#include "health/rolling_window.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace monitor::health {
namespace {

double MedianOf(std::vector<double> values) {
  if (values.empty()) return 0.0;
  const std::size_t middle = values.size() / 2;
  std::nth_element(values.begin(), values.begin() + middle, values.end());
  const double upper = values[middle];
  if (values.size() % 2 != 0) return upper;
  const double lower = *std::max_element(values.begin(), values.begin() + middle);
  return (lower + upper) / 2.0;
}

}  // namespace

RollingWindow::RollingWindow(std::size_t max_samples,
                             std::chrono::seconds max_age)
    : max_samples_(max_samples), max_age_(max_age) {}

bool RollingWindow::Push(double value, Clock::time_point timestamp) {
  if (!std::isfinite(value) || max_samples_ == 0 || max_age_.count() <= 0 ||
      (!samples_.empty() && timestamp < samples_.back().timestamp)) {
    return false;
  }
  Prune(timestamp);
  samples_.push_back({value, timestamp});
  while (samples_.size() > max_samples_) samples_.pop_front();
  return true;
}

void RollingWindow::Prune(Clock::time_point timestamp) {
  if (!samples_.empty() && timestamp < samples_.back().timestamp) return;
  while (!samples_.empty() && timestamp - samples_.front().timestamp > max_age_) {
    samples_.pop_front();
  }
}

double RollingWindow::Median() const {
  std::vector<double> values;
  values.reserve(samples_.size());
  for (const auto& sample : samples_) values.push_back(sample.value);
  return MedianOf(std::move(values));
}

double RollingWindow::Mad() const {
  if (samples_.empty()) return 0.0;
  const double median = Median();
  std::vector<double> deviations;
  deviations.reserve(samples_.size());
  for (const auto& sample : samples_) {
    deviations.push_back(std::abs(sample.value - median));
  }
  return MedianOf(std::move(deviations));
}

std::pair<double, double> RollingWindow::EwmaMeanStd(double alpha) const {
  if (samples_.empty() || !std::isfinite(alpha) || alpha <= 0.0 ||
      alpha > 1.0) {
    return {0.0, 0.0};
  }
  double mean = samples_.front().value;
  double variance = 0.0;
  for (std::size_t index = 1; index < samples_.size(); ++index) {
    const double delta = samples_[index].value - mean;
    mean += alpha * delta;
    variance = (1.0 - alpha) * (variance + alpha * delta * delta);
  }
  if (!std::isfinite(mean) || !std::isfinite(variance)) return {0.0, 0.0};
  return {mean, std::sqrt(std::max(0.0, variance))};
}

}  // namespace monitor::health
