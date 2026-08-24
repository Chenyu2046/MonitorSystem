#pragma once

#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>

#include "diagnostics/anomaly_detector.h"
#include "diagnostics/observability_config.h"
#include "monitor_info.pb.h"

namespace monitor::diagnostics {

/** @brief Thread-safe policy for accepting and merging Manager health scores. */
class RemoteHealthFeedback {
 public:
  using Clock = std::chrono::steady_clock;

  explicit RemoteHealthFeedback(std::chrono::milliseconds max_age);

  bool Accept(const monitor::proto::MonitorFeedback& feedback,
              const std::string& expected_host, std::int64_t now_unix_ms,
              Clock::time_point received_at = Clock::now());

  AnomalyResult Merge(
      const AnomalyResult& local, const std::string& expected_host,
      const ObservabilityConfig& config,
      Clock::time_point now = Clock::now()) const;

 private:
  const std::chrono::milliseconds max_age_;
  mutable std::mutex mutex_;
  std::string host_name_;
  std::uint64_t version_ = 0;
  std::int64_t result_timestamp_ms_ = 0;
  double health_anomaly_score_ = 0.0;
  double remote_trigger_score_ = 0.0;
  Clock::time_point received_at_ = Clock::time_point::min();
  bool valid_ = false;
};

}  // namespace monitor::diagnostics
