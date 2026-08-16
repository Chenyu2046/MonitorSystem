#include "diagnostics/probe_controller.h"

#include <utility>

namespace monitor::diagnostics {

bool ProbeController::Apply(ObservabilityState state) {
  auto desired = DesiredFor(state);
  if (initialized_ && desired == desired_probes_) {
    return true;
  }
  desired_probes_ = std::move(desired);
  initialized_ = true;
  ++apply_count_;
  return true;
}

std::set<ProbeKind> ProbeController::DesiredFor(ObservabilityState state) {
  switch (state) {
    case ObservabilityState::kNormal:
    case ObservabilityState::kCooldown:
      return {};
    case ObservabilityState::kSuspect:
      return {ProbeKind::kTcp, ProbeKind::kBlockIo};
    case ObservabilityState::kDiagnostic:
    case ObservabilityState::kProfiling:
      return {ProbeKind::kTcp, ProbeKind::kBlockIo, ProbeKind::kScheduler};
  }
  return {};
}

}  // namespace monitor::diagnostics
