#pragma once

/**
 * @file observability_config.h
 * @brief Worker 可观测性采样、阈值、队列和诊断时限配置。
 *
 * 配置同时约束普通监控周期、异常状态机状态转换、eBPF profiling 时长
 * 以及发送队列重试边界。它只描述策略参数，不保存运行时状态；运行时
 * 状态由 ObservabilityStateMachine、ProbeController 等对象分别持有。
 */

#include <cstddef>
#include <string>

namespace monitor::diagnostics {

/**
 * @brief 诊断控制平面的不可变参数快照。
 *
 * 分数范围是 0~1，时间/队列参数必须为正，阈值按 warning < critical
 * 和进入/恢复状态的顺序校验。环境变量解析在 MonitorPusher 中完成，
 * 本结构的 IsValid() 只负责一致性检查。
 */
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

  std::size_t sender_max_queue_items = 32;
  std::size_t sender_max_queue_bytes = 4 * 1024 * 1024;
  int sender_rpc_deadline_ms = 1500;
  int sender_max_retries = 3;
  int sender_retry_initial_ms = 100;
  int sender_retry_max_ms = 1000;

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

  /** @brief 校验时间、阈值、队列和状态机参数是否满足基本约束。 */
  bool IsValid() const;
};

}  // namespace monitor::diagnostics
