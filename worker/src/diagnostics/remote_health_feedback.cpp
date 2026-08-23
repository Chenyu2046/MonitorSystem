#include "diagnostics/remote_health_feedback.h"

#include <algorithm>
#include <cmath>

namespace monitor::diagnostics {

RemoteHealthFeedback::RemoteHealthFeedback(std::chrono::milliseconds max_age)
    : max_age_(max_age) {}

bool RemoteHealthFeedback::Accept(
    const monitor::proto::MonitorFeedback& feedback,
    const std::string& expected_host, std::int64_t now_unix_ms,
    Clock::time_point received_at) {
  constexpr std::int64_t kFutureSkewAllowanceMs = 5000;
  if (!feedback.health_valid() || expected_host.empty() ||
      feedback.host_name() != expected_host || feedback.result_version() == 0 ||
      feedback.result_timestamp_ms() <= 0 ||
      !std::isfinite(feedback.node_anomaly_score()) ||
      feedback.node_anomaly_score() < 0.0 ||
      feedback.node_anomaly_score() > 1.0 || max_age_.count() <= 0 ||
      feedback.result_timestamp_ms() >
          now_unix_ms + kFutureSkewAllowanceMs ||
      now_unix_ms - feedback.result_timestamp_ms() > max_age_.count()) {
    return false;
  }

  std::lock_guard<std::mutex> lock(mutex_);
  if (feedback.result_version() <= version_ ||
      feedback.result_timestamp_ms() < result_timestamp_ms_) {
    return false;
  }
  host_name_ = feedback.host_name();
  version_ = feedback.result_version();
  result_timestamp_ms_ = feedback.result_timestamp_ms();
  anomaly_score_ = feedback.node_anomaly_score();
  received_at_ = received_at;
  valid_ = true;
  return true;
}

AnomalyResult RemoteHealthFeedback::Merge(
    const AnomalyResult& local, const std::string& expected_host,
    const ObservabilityConfig& config, Clock::time_point now) const {
  AnomalyResult merged = local;
  const double local_score =
      std::isfinite(local.overall_score)
          ? std::clamp(local.overall_score, 0.0, 1.0)
          : 0.0;
  merged.overall_score = local_score;

  std::lock_guard<std::mutex> lock(mutex_);
  if (valid_ && host_name_ == expected_host && now >= received_at_ &&
      now - received_at_ <= max_age_) {
    // Remote absence or staleness can never lower a Worker-local anomaly.
    merged.overall_score = std::max(local_score, anomaly_score_);
  }
  merged.overall_score = std::clamp(merged.overall_score, 0.0, 1.0);
  merged.should_diagnose =
      local.should_diagnose ||
      merged.overall_score >= config.suspect_enter_score;
  merged.should_profile =
      local.should_profile ||
      merged.overall_score >= config.profiling_enter_score;
  return merged;
}

}  // namespace monitor::diagnostics
