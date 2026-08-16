#pragma once

#include <cstddef>
#include <set>

#include "diagnostics/observability_state.h"

namespace monitor::diagnostics {

enum class ProbeKind {
  kTcp,
  kBlockIo,
  kScheduler,
};

class ProbeController {
 public:
  // Phase 1 is a planner: real eBPF attach/detach starts in Phase 2.
  bool Apply(ObservabilityState state);

  const std::set<ProbeKind>& DesiredProbes() const { return desired_probes_; }
  std::size_t ApplyCount() const { return apply_count_; }

 private:
  static std::set<ProbeKind> DesiredFor(ObservabilityState state);

  std::set<ProbeKind> desired_probes_;
  std::size_t apply_count_ = 0;
  bool initialized_ = false;
};

}  // namespace monitor::diagnostics
