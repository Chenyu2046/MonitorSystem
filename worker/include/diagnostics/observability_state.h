#pragma once

/**
 * @file observability_state.h
 * @brief Worker 自适应采样和诊断状态机接口。
 *
 * 状态链路为 NORMAL -> SUSPECT -> DIAGNOSTIC -> PROFILING -> COOLDOWN。
 * 状态机消费一轮异常评分，使用连续异常/恢复样本和时间窗口抑制瞬时
 * 尖峰；它只管理策略状态，不直接启动线程或操作 eBPF。
 */

#include <chrono>

#include "diagnostics/anomaly_detector.h"

namespace monitor::diagnostics {

/** @brief 当前普通监控、诊断和 profiling 生命周期状态。 */
enum class ObservabilityState {
  kNormal,
  kSuspect,
  kDiagnostic,
  kProfiling,
  kCooldown,
};

/**
 * @brief 根据异常分数推进可观测性生命周期并选择采样间隔。
 *
 * anomaly_samples_、diagnostic_samples_ 和 recovery_samples_ 是连续样本
 * 计数器；进入 PROFILING/COOLDOWN 时记录时间点，用于限制额外开销和
 * 防止异常结束后立即重复 attach 探针。
 */
class ObservabilityStateMachine {
 public:
  using Clock = std::chrono::steady_clock;

  explicit ObservabilityStateMachine(ObservabilityConfig config = {});

  /**
   * @brief 用一轮异常结果推进状态机。
   * @return 状态是否发生转换。
   */
  bool Update(const AnomalyResult& result,
              Clock::time_point now = Clock::now());

  /** @brief 返回当前状态，供 ProbeController 和 protobuf 映射使用。 */
  ObservabilityState state() const { return state_; }
  /** @brief 返回当前状态对应的基础采样间隔毫秒数。 */
  int CurrentIntervalMs() const;

 private:
  void Enter(ObservabilityState state, Clock::time_point now);
  bool RecoveryConfirmed(const AnomalyResult& result);

  ObservabilityConfig config_;
  ObservabilityState state_ = ObservabilityState::kNormal;
  int anomaly_samples_ = 0;
  int diagnostic_samples_ = 0;
  int recovery_samples_ = 0;
  Clock::time_point profiling_started_ = Clock::time_point::min();
  Clock::time_point cooldown_started_ = Clock::time_point::min();
};

}  // namespace monitor::diagnostics
