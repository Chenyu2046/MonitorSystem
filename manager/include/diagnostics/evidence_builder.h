#pragma once

#include <chrono>
#include <string>
#include <vector>

#include "monitor_info.pb.h"

namespace monitor::diagnostics {

enum class EvidenceType {
  kCpuUsage,
  kRunQueue,
  kIoWait,
  kDiskUtil,
  kDiskLatency,
  kBpfBlockLatency,
  kNetPps,
  kTcpRetrans,
  kSoftirqNetRx,
  kSchedulerSwitches,
  kSchedulerWakeups,
  kMemoryAvailable,
  kOnCpuStack,
  kOffCpuStack,
};

struct Evidence {
  std::string id;
  EvidenceType type;
  std::string source;
  std::string target;
  double value = 0.0;
  std::string unit;
  double severity = 0.0;
  std::chrono::system_clock::time_point timestamp;
  std::string detail;
};

const char* EvidenceTypeName(EvidenceType type);

class EvidenceBuilder {
 public:
  std::vector<Evidence> Build(
      const monitor::proto::MonitorInfo& info,
      std::chrono::system_clock::time_point timestamp) const;
};

}  // namespace monitor::diagnostics
