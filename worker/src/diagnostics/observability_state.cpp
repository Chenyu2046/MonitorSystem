#include "diagnostics/observability_state.h"

#include <algorithm>
#include <utility>

namespace monitor::diagnostics {

ObservabilityStateMachine::ObservabilityStateMachine(ObservabilityConfig config)
    : config_(std::move(config)) {}

bool ObservabilityStateMachine::Update(const AnomalyResult& result,
                                       Clock::time_point now) {
  const auto previous = state_;

  switch (state_) {
    case ObservabilityState::kNormal:
      if (result.overall_score >= config_.suspect_enter_score) {
        ++anomaly_samples_;
        if (anomaly_samples_ >= config_.anomaly_enter_count) {
          Enter(ObservabilityState::kSuspect, now);
        }
      } else {
        anomaly_samples_ = 0;
      }
      break;

    case ObservabilityState::kSuspect:
      if (RecoveryConfirmed(result)) {
        if (recovery_samples_ >= config_.anomaly_exit_count) {
          Enter(ObservabilityState::kNormal, now);
        }
        break;
      }
      diagnostic_samples_ =
          result.overall_score >= config_.diagnostic_enter_score
              ? diagnostic_samples_ + 1
              : 0;
      if (diagnostic_samples_ >= config_.diagnostic_enter_count) {
        Enter(ObservabilityState::kDiagnostic, now);
      }
      break;

    case ObservabilityState::kDiagnostic:
      if (RecoveryConfirmed(result)) {
        if (recovery_samples_ >= config_.anomaly_exit_count) {
          Enter(ObservabilityState::kCooldown, now);
        }
        break;
      }
      if (result.should_profile &&
          result.overall_score >= config_.profiling_enter_score) {
        Enter(ObservabilityState::kProfiling, now);
      }
      break;

    case ObservabilityState::kProfiling: {
      const auto max_duration = std::chrono::seconds(std::min(
          config_.profiling_duration_sec, config_.profiling_max_duration_sec));
      if (now - profiling_started_ >= max_duration ||
          RecoveryConfirmed(result)) {
        Enter(ObservabilityState::kCooldown, now);
      }
      break;
    }

    case ObservabilityState::kCooldown: {
      const auto cooldown = std::chrono::seconds(config_.cooldown_sec);
      if (RecoveryConfirmed(result)) {
        if (now - cooldown_started_ >= cooldown &&
            recovery_samples_ >= config_.anomaly_exit_count) {
          Enter(ObservabilityState::kNormal, now);
        }
        break;
      }

      // Do not carry anomalous samples collected during the cooldown window
      // into the next diagnostic episode. A persistent anomaly must be
      // observed again after cooldown has elapsed.
      if (now - cooldown_started_ < cooldown) {
        diagnostic_samples_ = 0;
        break;
      }

      diagnostic_samples_ =
          result.overall_score >= config_.diagnostic_enter_score
              ? diagnostic_samples_ + 1
              : 0;
      if (now - cooldown_started_ >= cooldown &&
          diagnostic_samples_ >= config_.diagnostic_enter_count) {
        Enter(ObservabilityState::kDiagnostic, now);
      }
      break;
    }
  }

  return previous != state_;
}

int ObservabilityStateMachine::CurrentIntervalMs() const {
  switch (state_) {
    case ObservabilityState::kNormal:
      return config_.normal_interval_ms;
    case ObservabilityState::kSuspect:
      return config_.suspect_interval_ms;
    case ObservabilityState::kDiagnostic:
    case ObservabilityState::kProfiling:
    case ObservabilityState::kCooldown:
      return config_.diagnostic_interval_ms;
  }
  return config_.normal_interval_ms;
}

void ObservabilityStateMachine::Enter(ObservabilityState state,
                                      Clock::time_point now) {
  state_ = state;
  anomaly_samples_ = 0;
  diagnostic_samples_ = 0;
  recovery_samples_ = 0;
  if (state == ObservabilityState::kProfiling) {
    profiling_started_ = now;
  }
  if (state == ObservabilityState::kCooldown) {
    cooldown_started_ = now;
  }
}

bool ObservabilityStateMachine::RecoveryConfirmed(const AnomalyResult& result) {
  if (result.overall_score <= config_.recovery_score) {
    ++recovery_samples_;
    return true;
  }
  recovery_samples_ = 0;
  return false;
}

}  // namespace monitor::diagnostics
