/**
 * @file observability_config.cpp
 * @brief 可观测性配置的一致性校验实现。
 *
 * 校验结果用于阻止不可能的状态机阈值、负间隔、无界 profiling 或错误
 * 重试窗口进入运行时；本文件不修改配置，也不读取环境变量。
 */

#include "diagnostics/observability_config.h"

namespace monitor::diagnostics {

bool ObservabilityConfig::IsValid() const {
  // 所有比较集中在这里，确保状态机、ProbeController 和发送队列看到的
  // 参数处于同一组基本不变量内；具体业务阈值仍由调用方解释。
  return normal_interval_ms >= 1000 && suspect_interval_ms >= 1000 &&
         diagnostic_interval_ms >= 1000 && anomaly_enter_count > 0 &&
         diagnostic_enter_count > 0 && anomaly_exit_count > 0 &&
         profiling_duration_sec > 0 && profiling_max_duration_sec > 0 &&
         profiling_sample_hz > 0 && cooldown_sec > 0 &&
         profiling_duration_sec <= profiling_max_duration_sec &&
         sender_max_queue_items > 0 && sender_max_queue_bytes > 0 &&
         sender_rpc_deadline_ms > 0 && sender_max_retries >= 0 &&
         sender_retry_initial_ms > 0 &&
         sender_retry_max_ms >= sender_retry_initial_ms &&
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
         network_pps_warning < network_pps_critical &&
#ifdef ENABLE_EBPF
         !ebpf_object_dir.empty();
#else
         true;
#endif
}

}  // namespace monitor::diagnostics
