#pragma once

#include <string>
#include <vector>

#include "diagnostics/evidence_builder.h"

namespace monitor::diagnostics {

enum class RootCauseType {
  kCpuSaturation,
  kDiskIoSaturation,
  kNetworkStackPressure,
  kMemoryPressure,
  kLockContention,
  kUnknown,
};

struct RootCause {
  RootCauseType type = RootCauseType::kUnknown;
  double confidence = 0.0;
  std::vector<std::string> evidence_ids;
  std::string summary;
};

const char* RootCauseTypeName(RootCauseType type);

class RootCauseEngine {
 public:
  std::vector<RootCause> Evaluate(const std::vector<Evidence>& evidence) const;
};

}  // namespace monitor::diagnostics
