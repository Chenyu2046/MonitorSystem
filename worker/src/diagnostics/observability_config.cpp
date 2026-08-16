#include "diagnostics/observability_config.h"

namespace monitor::diagnostics {

bool ObservabilityConfig::IsValid() const {
  return normal_interval_ms > 0 && suspect_interval_ms > 0 &&
         diagnostic_interval_ms > 0 && anomaly_enter_count > 0 &&
         diagnostic_enter_count > 0 && anomaly_exit_count > 0 &&
         profiling_duration_sec > 0 && profiling_max_duration_sec > 0 &&
         cooldown_sec > 0 &&
         profiling_duration_sec <= profiling_max_duration_sec &&
         suspect_enter_score > recovery_score &&
         diagnostic_enter_score > suspect_enter_score &&
         profiling_enter_score >= diagnostic_enter_score &&
         profiling_enter_score <= 1.0 && recovery_score >= 0.0 &&
         suspect_enter_score >= 0.0 && suspect_enter_score <= 1.0 &&
         diagnostic_enter_score >= 0.0 && diagnostic_enter_score <= 1.0 &&
         cpu_warning_percent < cpu_critical_percent &&
         io_wait_warning_percent < io_wait_critical_percent &&
         softirq_warning_percent < softirq_critical_percent &&
         softirq_warning_per_sec < softirq_critical_per_sec &&
         load_warning < load_critical &&
         memory_warning_percent < memory_critical_percent &&
         disk_util_warning_percent < disk_util_critical_percent &&
         disk_latency_warning_ms < disk_latency_critical_ms &&
         network_pps_warning < network_pps_critical;
}

}  // namespace monitor::diagnostics
