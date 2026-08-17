#pragma once

#include <string>
#include <vector>

#include "diagnostics/observability_config.h"

#include "monitor_info.pb.h"

namespace monitor::diagnostics {

enum class AnomalyDomain {
  kCpu,
  kMemory,
  kDisk,
  kNetwork,
  kScheduler,
};

struct AnomalySignal {
  AnomalyDomain domain;
  std::string metric;
  double value = 0.0;
  double score = 0.0;
  bool triggered = false;
};

struct AnomalyResult {
  double overall_score = 0.0;
  std::vector<AnomalySignal> signals;
  bool should_diagnose = false;
  bool should_profile = false;
};

class AnomalyDetector {
 public:
  explicit AnomalyDetector(ObservabilityConfig config = {});

  AnomalyResult Evaluate(const monitor::proto::MonitorInfo& info) const;

 private:
  ObservabilityConfig config_;
};

}  // namespace monitor::diagnostics
