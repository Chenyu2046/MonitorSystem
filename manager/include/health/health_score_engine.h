#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <deque>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "health/health_types.h"
#include "health/metric_detector.h"
#include "health/rolling_window.h"
#include "monitor_info.pb.h"

namespace monitor::health {

struct HealthConfig {
  std::size_t max_samples = 60;
  std::chrono::seconds window_age{600};
  std::size_t min_samples = 30;
  double mad_warning_z = 3.5;
  double mad_critical_z = 6.0;
  double ewma_alpha = 0.2;
  double ewma_warning_sigma = 2.0;
  double ewma_critical_sigma = 4.0;
  int consensus_min_votes = 2;
  double cpu_domain_weight = 0.30;
  double memory_domain_weight = 0.20;
  double disk_domain_weight = 0.25;
  double network_domain_weight = 0.15;
  double scheduler_domain_weight = 0.10;
  std::chrono::seconds nar_window{300};
  std::size_t max_nar_frames = 600;

  bool IsValid() const;
};

bool LoadHealthConfigFromEnvironment(HealthConfig* config,
                                     std::string* error);

class HealthScoreEngine {
 public:
  using Clock = std::chrono::steady_clock;

  explicit HealthScoreEngine(HealthConfig config = {});

  HealthResult Evaluate(const monitor::proto::MonitorInfo& info,
                        Clock::time_point timestamp, double resource_score);
  std::optional<Clock::time_point> LastTimestamp() const {
    return last_timestamp_;
  }

 private:
  struct AnomalyFrame {
    Clock::time_point timestamp;
    std::size_t anomalous_metrics = 0;
    std::size_t valid_metrics = 0;
  };

  struct NetworkCounters {
    long double drops = 0.0;
    long double errors = 0.0;
    Clock::time_point timestamp;
  };

  HealthConfig config_;
  MetricDetector detector_;
  std::vector<RollingWindow> windows_;
  std::deque<AnomalyFrame> anomaly_history_;
  std::optional<NetworkCounters> network_counters_;
  std::optional<Clock::time_point> last_timestamp_;
};

std::vector<std::string> PruneStaleHealthEngines(
    std::unordered_map<std::string, HealthScoreEngine>* engines,
    HealthScoreEngine::Clock::time_point now,
    HealthScoreEngine::Clock::duration max_idle);

}  // namespace monitor::health
