#pragma once

#include <cstddef>
#include <chrono>
#include <optional>

#include "health/health_types.h"
#include "health/rolling_window.h"

namespace monitor::health {

struct DetectorConfig {
  std::size_t ready_samples = 30;
  double mad_warning_z = 3.5;
  double mad_critical_z = 6.0;
  double ewma_alpha = 0.2;
  double ewma_warning_sigma = 2.0;
  double ewma_critical_sigma = 4.0;
  int consensus_min_votes = 2;
  std::chrono::seconds minimum_history_duration{300};
};

struct StaticThreshold {
  double warning = 0.0;
  double critical = 0.0;
};

class MetricDetector {
 public:
  explicit MetricDetector(DetectorConfig config) : config_(config) {}

  DetectorResult Evaluate(
      double value, std::optional<StaticThreshold> threshold,
      const RollingWindow& history,
      RollingWindow::Clock::time_point timestamp) const;

 private:
  DetectorConfig config_;
};

}  // namespace monitor::health
