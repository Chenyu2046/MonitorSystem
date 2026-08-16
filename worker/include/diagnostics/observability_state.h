#pragma once

#include <chrono>

#include "diagnostics/anomaly_detector.h"

namespace monitor::diagnostics {

enum class ObservabilityState {
  kNormal,
  kSuspect,
  kDiagnostic,
  kProfiling,
  kCooldown,
};

class ObservabilityStateMachine {
 public:
  using Clock = std::chrono::steady_clock;

  explicit ObservabilityStateMachine(ObservabilityConfig config = {});

  bool Update(const AnomalyResult& result,
              Clock::time_point now = Clock::now());

  ObservabilityState state() const { return state_; }
  int CurrentIntervalMs() const;

 private:
  void Enter(ObservabilityState state, Clock::time_point now);
  bool RecoveryConfirmed(const AnomalyResult& result);

  ObservabilityConfig config_;
  ObservabilityState state_ = ObservabilityState::kNormal;
  int anomaly_samples_ = 0;
  int diagnostic_samples_ = 0;
  int recovery_samples_ = 0;
  Clock::time_point profiling_started_ = Clock::time_point::min();
  Clock::time_point cooldown_started_ = Clock::time_point::min();
};

}  // namespace monitor::diagnostics
