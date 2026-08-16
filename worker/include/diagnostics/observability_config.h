#pragma once

#include <string>

namespace monitor::diagnostics {

struct ObservabilityConfig {
  int normal_interval_ms = 10000;
  int suspect_interval_ms = 2000;
  int diagnostic_interval_ms = 1000;

  int anomaly_enter_count = 2;
  int diagnostic_enter_count = 2;
  int anomaly_exit_count = 3;

  double suspect_enter_score = 0.60;
  double diagnostic_enter_score = 0.80;
  double profiling_enter_score = 0.95;
  double recovery_score = 0.35;

  int profiling_duration_sec = 15;
  int profiling_max_duration_sec = 30;
  int profiling_sample_hz = 49;
  int cooldown_sec = 30;
  std::string ebpf_object_dir = "worker/src/ebpf/.output";

  double cpu_warning_percent = 70.0;
  double cpu_critical_percent = 90.0;
  double io_wait_warning_percent = 10.0;
  double io_wait_critical_percent = 30.0;
  double softirq_warning_percent = 10.0;
  double softirq_critical_percent = 30.0;
  double softirq_warning_per_sec = 10000.0;
  double softirq_critical_per_sec = 100000.0;
  double load_warning = 1.0;
  double load_critical = 4.0;
  double memory_warning_percent = 80.0;
  double memory_critical_percent = 95.0;
  double disk_util_warning_percent = 70.0;
  double disk_util_critical_percent = 95.0;
  double disk_latency_warning_ms = 10.0;
  double disk_latency_critical_ms = 40.0;
  double network_pps_warning = 10000.0;
  double network_pps_critical = 100000.0;

  bool IsValid() const;
};

}  // namespace monitor::diagnostics
