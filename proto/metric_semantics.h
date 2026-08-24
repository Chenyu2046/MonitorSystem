#pragma once

// One definition of threshold units and aggregation semantics shared by the
// Worker rule detector and Manager adaptive model.
namespace monitor::metric_semantics {

struct MetricThreshold {
  double warning;
  double critical;
};

inline constexpr MetricThreshold kCpuPercent{70.0, 90.0};
inline constexpr MetricThreshold kIoWaitPercent{10.0, 30.0};
inline constexpr MetricThreshold kSoftIrqPercent{10.0, 30.0};
inline constexpr MetricThreshold kLoadPerCpu{1.0, 4.0};
inline constexpr MetricThreshold kMemoryPercent{80.0, 95.0};
inline constexpr MetricThreshold kDiskUtilPercent{70.0, 95.0};
inline constexpr MetricThreshold kDiskLatencyMs{10.0, 40.0};
inline constexpr MetricThreshold kNetworkPps{10000.0, 100000.0};
inline constexpr MetricThreshold kNetworkSoftIrqPerSec{10000.0, 100000.0};

}  // namespace monitor::metric_semantics
