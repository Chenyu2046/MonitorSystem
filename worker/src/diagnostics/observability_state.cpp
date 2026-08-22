/**
 * @file observability_state.cpp
 * @brief 自适应采样状态机的状态转换和计时实现。
 *
 * 连续高分样本推动 NORMAL/SUSPECT/DIAGNOSTIC，profiling 受最大时长
 * 限制，恢复后进入 COOLDOWN，冷却结束且连续恢复样本足够才回到 NORMAL。
 * 这样普通采集路径不会因单次尖峰直接启动重型诊断。
 */

#include "diagnostics/observability_state.h"

#include <algorithm>
#include <utility>

namespace monitor::diagnostics {

ObservabilityStateMachine::ObservabilityStateMachine(ObservabilityConfig config)
    : config_(std::move(config)) {}

bool ObservabilityStateMachine::Update(const AnomalyResult& result,
                                       Clock::time_point now) {
  // 保留转换前状态用于返回值；调用方据此可观察状态变化，但不需要
  // 额外锁，因为状态机由 MonitorPusher 的 PushLoop 单线程推进。
  const auto previous = state_;

  switch (state_) {
    case ObservabilityState::kNormal:
      // NORMAL -> SUSPECT 需要连续达到 suspect_enter_score 的样本，避免
      // 一次瞬时尖峰立即降低采样间隔并开启诊断探针。
      if (result.overall_score >= config_.suspect_enter_score) {
        ++anomaly_samples_;
        if (anomaly_samples_ >= config_.anomaly_enter_count) {
          Enter(ObservabilityState::kSuspect, now);
        }
      } else {
        anomaly_samples_ = 0;
      }
      break;

    case ObservabilityState::kSuspect:
      // SUSPECT 先检查连续恢复；未恢复时再累计更严重的诊断样本，保证
      // 轻微异常不会直接跳过中间状态。
      if (RecoveryConfirmed(result)) {
        if (recovery_samples_ >= config_.anomaly_exit_count) {
          Enter(ObservabilityState::kNormal, now);
        }
        break;
      }
      diagnostic_samples_ =
          result.overall_score >= config_.diagnostic_enter_score
              ? diagnostic_samples_ + 1
              : 0;
      if (diagnostic_samples_ >= config_.diagnostic_enter_count) {
        Enter(ObservabilityState::kDiagnostic, now);
      }
      break;

    case ObservabilityState::kDiagnostic:
      // DIAGNOSTIC 已开启基础诊断 Probe；只有持续达到 profiling 阈值，
      // 才升级为有明确时长上限的 PROFILING。
      if (RecoveryConfirmed(result)) {
        if (recovery_samples_ >= config_.anomaly_exit_count) {
          Enter(ObservabilityState::kCooldown, now);
        }
        break;
      }
      if (result.should_profile &&
          result.overall_score >= config_.profiling_enter_score) {
        Enter(ObservabilityState::kProfiling, now);
      }
      break;

    case ObservabilityState::kProfiling: {
      // PROFILING 到时或异常恢复后进入 COOLDOWN，防止 profiling 无限运行
      // 或刚结束就被同一异常重复启动。
      const auto max_duration = std::chrono::seconds(std::min(
          config_.profiling_duration_sec, config_.profiling_max_duration_sec));
      if (now - profiling_started_ >= max_duration ||
          RecoveryConfirmed(result)) {
        Enter(ObservabilityState::kCooldown, now);
      }
      break;
    }

    case ObservabilityState::kCooldown: {
      // COOLDOWN 保留较快采样但抑制重型探针。持续异常必须在冷却窗口
      // 之后重新累计，避免把旧 episode 的样本直接延续到下一次诊断。
      const auto cooldown = std::chrono::seconds(config_.cooldown_sec);
      if (RecoveryConfirmed(result)) {
        if (now - cooldown_started_ >= cooldown &&
            recovery_samples_ >= config_.anomaly_exit_count) {
          Enter(ObservabilityState::kNormal, now);
        }
        break;
      }

      // Do not carry anomalous samples collected during the cooldown window
      // into the next diagnostic episode. A persistent anomaly must be
      // observed again after cooldown has elapsed.
      if (now - cooldown_started_ < cooldown) {
        diagnostic_samples_ = 0;
        break;
      }

      diagnostic_samples_ =
          result.overall_score >= config_.diagnostic_enter_score
              ? diagnostic_samples_ + 1
              : 0;
      if (now - cooldown_started_ >= cooldown &&
          diagnostic_samples_ >= config_.diagnostic_enter_count) {
        Enter(ObservabilityState::kDiagnostic, now);
      }
      break;
    }
  }

  return previous != state_;
}

int ObservabilityStateMachine::CurrentIntervalMs() const {
  switch (state_) {
    case ObservabilityState::kNormal:
      return config_.normal_interval_ms;
    case ObservabilityState::kSuspect:
      return config_.suspect_interval_ms;
    case ObservabilityState::kDiagnostic:
    case ObservabilityState::kProfiling:
    case ObservabilityState::kCooldown:
      return config_.diagnostic_interval_ms;
  }
  return config_.normal_interval_ms;
}

void ObservabilityStateMachine::Enter(ObservabilityState state,
                                      Clock::time_point now) {
  // 每次进入新状态都清空上一状态的连续样本计数；profiling/cooldown
  // 额外记录起始时间，供后续状态判断生命周期。
  state_ = state;
  anomaly_samples_ = 0;
  diagnostic_samples_ = 0;
  recovery_samples_ = 0;
  if (state == ObservabilityState::kProfiling) {
    profiling_started_ = now;
  }
  if (state == ObservabilityState::kCooldown) {
    cooldown_started_ = now;
  }
}

bool ObservabilityStateMachine::RecoveryConfirmed(const AnomalyResult& result) {
  // 恢复也要求连续样本，避免一次低分采样把仍在抖动的异常状态提前
  // 关闭；分数回升则重新从零累计。
  if (result.overall_score <= config_.recovery_score) {
    ++recovery_samples_;
    return true;
  }
  recovery_samples_ = 0;
  return false;
}

}  // namespace monitor::diagnostics
