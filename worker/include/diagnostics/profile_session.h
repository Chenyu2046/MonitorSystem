#pragma once

/**
 * @file profile_session.h
 * @brief 单次 OnCPU/OffCPU profiling 生命周期模型。
 *
 * ProfileSession 只保存 id、类型、目标 PID、开始时间和最大时长，并在
 * Close() 时通过回调通知 ProbeController detach；它不实现采样、不解析
 * stack trace，也不把 samples 数量解释为精确 CPU 百分比。
 */

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>

namespace monitor::diagnostics {

/** @brief profiling 的采样方向。 */
enum class ProfileType {
  kOnCpu,
  kOffCpu,
};

/**
 * @brief 约束一次 profiling 的开始、过期和关闭状态。
 */
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

  /** @brief 启动尚未 active 的 session 并记录开始时间。 */
  void Start(Clock::time_point now = Clock::now());
  /** @brief 判断 active session 是否达到最大持续时间。 */
  bool Expired(Clock::time_point now = Clock::now()) const;
  /** @brief 幂等关闭 session，并触发 Probe detach 回调。 */
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
