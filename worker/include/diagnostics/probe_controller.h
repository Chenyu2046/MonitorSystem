#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <set>
#include <string>

#include "diagnostics/diagnostic_snapshot.h"
#include "diagnostics/observability_state.h"
#include "diagnostics/profile_session.h"

namespace monitor::diagnostics {

enum class ProbeKind {
  kTcp,
  kBlockIo,
  kScheduler,
  kOnCpuProfile,
  kOffCpuProfile,
};

class ProbeController {
 public:
  explicit ProbeController(std::string object_dir = {},
                           int profile_sample_hz = 49,
                           int profile_max_duration_sec = 30);
  ~ProbeController();

  bool Apply(
      ObservabilityState state, ProfileType profile_type = ProfileType::kOnCpu,
      ProfileSession::Clock::time_point now = ProfileSession::Clock::now());
  bool CollectSnapshot(DiagnosticSnapshot* snapshot) const;

  const std::set<ProbeKind>& DesiredProbes() const { return desired_probes_; }
  std::size_t ApplyCount() const { return apply_count_; }

  struct ProbeStatus {
    bool requested = false;
    bool available = false;
    bool attached = false;
    int last_error = 0;
    std::chrono::steady_clock::time_point next_retry_at =
        std::chrono::steady_clock::time_point::min();
    std::uint32_t retry_count = 0;
  };

  const ProbeStatus& Status(ProbeKind kind) const;

 private:
  struct Runtime;

  static std::size_t Index(ProbeKind kind);
  static std::set<ProbeKind> DesiredFor(ObservabilityState state,
                                        ProfileType profile_type,
                                        bool profile_active);
  void DetachProfile();

  std::string object_dir_;
  int profile_sample_hz_;
  std::chrono::seconds profile_max_duration_;
  std::set<ProbeKind> desired_probes_;
  std::array<ProbeStatus, 5> statuses_{};
  std::size_t apply_count_ = 0;
  bool initialized_ = false;
  std::unique_ptr<Runtime> runtime_;
  std::uint64_t next_profile_id_ = 1;
  std::unique_ptr<ProfileSession> profile_session_;
  bool profile_expired_ = false;
};

}  // namespace monitor::diagnostics
