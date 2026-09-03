#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace monitor::health {

enum class ModelState { kCold, kWarming, kReady };

enum class MetricId {
  kCpuAverage,
  kCpuPeak,
  kIoWait,
  kSoftIrqPercent,
  kLoadPerCpu,
  kMemoryUsed,
  kDiskUtil,
  kDiskLatency,
  kDiskIops,
  kNetworkPps,
  kNetworkRxThroughput,
  kNetworkTxThroughput,
  kNetworkDropsPerSec,
  kNetworkErrorsPerSec,
  kNetworkSoftIrqPerSec,
};

enum class Domain { kCpu, kMemory, kDisk, kNetwork, kScheduler };

struct DetectorResult {
  bool threshold_available = false;
  double threshold_score = 0.0;
  double mad_score = 0.0;
  double ewma_score = 0.0;
  int anomaly_votes = 0;
  double anomaly_score = 0.0;
  bool anomalous = false;
  ModelState model_state = ModelState::kCold;
};

struct TopSignal {
  MetricId metric = MetricId::kCpuAverage;
  double value = 0.0;
  DetectorResult detector;
};

struct HealthResult {
  double health_score = 0.0;
  double resource_score = 0.0;
  double anomaly_score = 0.0;
  double remote_trigger_score = 0.0;
  double anomaly_rate_5m = 0.0;
  double cpu_score = 0.0;
  double memory_score = 0.0;
  double disk_score = 0.0;
  double network_score = 0.0;
  double scheduler_score = 0.0;
  double confidence = 0.0;
  ModelState model_state = ModelState::kCold;
  std::string state = "UNKNOWN";
  std::vector<TopSignal> top_signals;
  bool valid = false;
};

// 返回指标的稳定文本名称，用于日志、RPC 和持久化。
const char* MetricName(MetricId metric);
// 返回模型状态的稳定文本名称。
const char* ModelStateName(ModelState state);

}  // namespace monitor::health
