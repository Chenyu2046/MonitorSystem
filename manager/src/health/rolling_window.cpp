#include "health/rolling_window.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace monitor::health {
namespace {

double MedianOf(std::vector<double> values) {
  // 对样本副本求中位数，避免改变滚动窗口中的原始顺序。
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
    // 窗口同时受样本数量和样本时间跨度限制。
    : max_samples_(max_samples), max_age_(max_age) {}

bool RollingWindow::Push(double value, Clock::time_point timestamp) {
  // 拒绝无效或乱序样本，再按当前事件时间淘汰过期数据并追加样本。
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
  // 只允许沿事件时间向前清理，避免回退时间误删仍然有效的历史。
  if (!samples_.empty() && timestamp < samples_.back().timestamp) return;
  while (!samples_.empty() && timestamp - samples_.front().timestamp > max_age_) {
    samples_.pop_front();
  }
}

std::chrono::seconds RollingWindow::Age(Clock::time_point timestamp) const {
  // 返回当前时间相对最早样本的跨度，空窗口或时间倒退时按零处理。
  if (samples_.empty() || timestamp < samples_.front().timestamp) {
    return std::chrono::seconds::zero();
  }
  return std::chrono::duration_cast<std::chrono::seconds>(
      timestamp - samples_.front().timestamp);
}

double RollingWindow::Median() const {
  // 提取窗口值并计算稳健的中心位置，供 MAD 等检测指标使用。
  std::vector<double> values;
  values.reserve(samples_.size());
  for (const auto& sample : samples_) values.push_back(sample.value);
  return MedianOf(std::move(values));
}

double RollingWindow::Mad() const {
  // 计算样本到中位数的绝对偏差中位数，降低离群点对基线的影响。
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
  // 按时间顺序计算指数加权均值和标准差，突出较新的观测值。
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
