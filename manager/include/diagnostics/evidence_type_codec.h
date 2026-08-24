#pragma once

#include <array>
#include <optional>
#include <string_view>
#include <utility>

#include "diagnostics/evidence_builder.h"

namespace monitor::diagnostics {

/** @brief 将持久化 evidence_type 名称安全还原为内部枚举。 */
inline std::optional<EvidenceType> ParseEvidenceType(std::string_view name) {
  static constexpr std::array<std::pair<std::string_view, EvidenceType>, 22>
      kNames{{
          {"cpu_usage", EvidenceType::kCpuUsage},
          {"run_queue", EvidenceType::kRunQueue},
          {"io_wait", EvidenceType::kIoWait},
          {"disk_util", EvidenceType::kDiskUtil},
          {"disk_latency", EvidenceType::kDiskLatency},
          {"bpf_block_latency", EvidenceType::kBpfBlockLatency},
          {"net_pps", EvidenceType::kNetPps},
          {"tcp_retrans", EvidenceType::kTcpRetrans},
          {"softirq_net_rx", EvidenceType::kSoftirqNetRx},
          {"scheduler_switches", EvidenceType::kSchedulerSwitches},
          {"scheduler_wakeups", EvidenceType::kSchedulerWakeups},
          {"diagnostic_capability_degraded",
           EvidenceType::kDiagnosticCapabilityDegraded},
          {"memory_available", EvidenceType::kMemoryAvailable},
          {"oncpu_stack", EvidenceType::kOnCpuStack},
          {"offcpu_stack", EvidenceType::kOffCpuStack},
          {"lock_wait_stack", EvidenceType::kLockWaitStack},
          {"health_anomaly_signal", EvidenceType::kHealthAnomalySignal},
          {"health_cpu_signal", EvidenceType::kHealthCpuSignal},
          {"health_memory_signal", EvidenceType::kHealthMemorySignal},
          {"health_disk_signal", EvidenceType::kHealthDiskSignal},
          {"health_network_signal", EvidenceType::kHealthNetworkSignal},
          {"health_scheduler_signal", EvidenceType::kHealthSchedulerSignal},
      }};
  for (const auto& [encoded, type] : kNames) {
    if (encoded == name) return type;
  }
  return std::nullopt;
}

}  // namespace monitor::diagnostics
