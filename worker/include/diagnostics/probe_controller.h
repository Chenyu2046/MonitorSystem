#pragma once

#include <array>
#include <cstddef>
#include <memory>
#include <set>
#include <string>

#include "diagnostics/diagnostic_snapshot.h"
#include "diagnostics/observability_state.h"

namespace monitor::diagnostics {

enum class ProbeKind {
  kTcp,
  kBlockIo,
  kScheduler,
};

class ProbeController {
 public:
  explicit ProbeController(std::string object_dir = {});
  ~ProbeController();

  bool Apply(ObservabilityState state);
  bool CollectSnapshot(DiagnosticSnapshot* snapshot) const;

  const std::set<ProbeKind>& DesiredProbes() const { return desired_probes_; }
  std::size_t ApplyCount() const { return apply_count_; }

  struct ProbeStatus {
    bool requested = false;
    bool available = false;
    bool attached = false;
    int last_error = 0;
  };

  const ProbeStatus& Status(ProbeKind kind) const;

 private:
  struct Runtime;

  static std::size_t Index(ProbeKind kind);
  static std::set<ProbeKind> DesiredFor(ObservabilityState state);

  std::string object_dir_;
  std::set<ProbeKind> desired_probes_;
  std::array<ProbeStatus, 3> statuses_{};
  std::size_t apply_count_ = 0;
  bool initialized_ = false;
  std::unique_ptr<Runtime> runtime_;
};

}  // namespace monitor::diagnostics
