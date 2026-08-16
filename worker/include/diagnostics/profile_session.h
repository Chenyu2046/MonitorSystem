#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>

namespace monitor::diagnostics {

enum class ProfileType {
  kOnCpu,
  kOffCpu,
};

class ProfileSession {
 public:
  using Clock = std::chrono::steady_clock;

  ProfileSession(std::uint64_t id, ProfileType type,
                 std::chrono::seconds max_duration,
                 std::optional<int> target_pid = std::nullopt,
                 std::function<void()> detach = {});
  ~ProfileSession();

  ProfileSession(const ProfileSession&) = delete;
  ProfileSession& operator=(const ProfileSession&) = delete;

  void Start(Clock::time_point now = Clock::now());
  bool Expired(Clock::time_point now = Clock::now()) const;
  void Close();

  std::uint64_t id() const { return id_; }
  ProfileType type() const { return type_; }
  bool active() const { return active_; }
  const std::optional<int>& target_pid() const { return target_pid_; }

 private:
  std::uint64_t id_;
  ProfileType type_;
  Clock::time_point start_ = Clock::time_point::min();
  std::chrono::seconds max_duration_;
  std::optional<int> target_pid_;
  std::function<void()> detach_;
  bool active_ = false;
};

}  // namespace monitor::diagnostics
