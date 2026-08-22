/**
 * @file profile_session.cpp
 * @brief Profiling session 的生命周期实现。
 */

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
  // Start 幂等，避免状态机重复调用时重置 profiling 起始时间。
  if (active_) {
    return;
  }
  start_ = now;
  active_ = true;
}

bool ProfileSession::Expired(Clock::time_point now) const {
  // 只对 active session 判断时长；未启动 session 不应触发 detach。
  return active_ && now - start_ >= max_duration_;
}

void ProfileSession::Close() {
  // 先切换 active 状态再调用回调，保证回调再次观察 session 时不会
  // 把正在关闭的 session 当作有效 profiling。
  if (!active_) {
    return;
  }
  active_ = false;
  if (detach_) {
    detach_();
  }
}

}  // namespace monitor::diagnostics
