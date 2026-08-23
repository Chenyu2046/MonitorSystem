#include "health/health_types.h"

namespace monitor::health {

const char* MetricName(MetricId metric) {
  switch (metric) {
    case MetricId::kCpuAverage:
      return "cpu_average_percent";
    case MetricId::kCpuPeak:
      return "cpu_peak_percent";
    case MetricId::kIoWait:
      return "io_wait_percent";
    case MetricId::kSoftIrqPercent:
      return "softirq_percent";
    case MetricId::kLoadPerCpu:
      return "load_per_cpu";
    case MetricId::kMemoryUsed:
      return "memory_used_percent";
    case MetricId::kDiskUtil:
      return "disk_util_percent";
    case MetricId::kDiskLatency:
      return "disk_latency_ms";
    case MetricId::kDiskIops:
      return "disk_iops";
    case MetricId::kNetworkPps:
      return "network_packets_per_sec";
    case MetricId::kNetworkRxThroughput:
      return "network_rx_kib_per_sec";
    case MetricId::kNetworkTxThroughput:
      return "network_tx_kib_per_sec";
    case MetricId::kNetworkDropsPerSec:
      return "network_drops_per_sec";
    case MetricId::kNetworkErrorsPerSec:
      return "network_errors_per_sec";
    case MetricId::kNetworkSoftIrqPerSec:
      return "network_softirq_per_sec";
  }
  return "unknown";
}

const char* ModelStateName(ModelState state) {
  switch (state) {
    case ModelState::kCold:
      return "COLD";
    case ModelState::kWarming:
      return "WARMING";
    case ModelState::kReady:
      return "READY";
  }
  return "COLD";
}

}  // namespace monitor::health
