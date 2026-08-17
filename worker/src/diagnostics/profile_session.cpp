#include "diagnostics/profile_session.h"

#include <utility>

namespace monitor::diagnostics {

ProfileSession::ProfileSession(std::uint64_t id, ProfileType type,
                               std::chrono::seconds max_duration,
                               std::optional<int> target_pid,
                               std::function<void()> detach)
    : id_(id),
      type_(type),
      max_duration_(max_duration),
      target_pid_(target_pid),
      detach_(std::move(detach)) {}

ProfileSession::~ProfileSession() { Close(); }

void ProfileSession::Start(Clock::time_point now) {
  if (active_) {
    return;
  }
  start_ = now;
  active_ = true;
}

bool ProfileSession::Expired(Clock::time_point now) const {
  return active_ && now - start_ >= max_duration_;
}

void ProfileSession::Close() {
  if (!active_) {
    return;
  }
  active_ = false;
  if (detach_) {
    detach_();
  }
}

}  // namespace monitor::diagnostics
