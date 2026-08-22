#pragma once

/**
 * @file anomaly_detector.h
 * @brief Worker 基础指标异常信号和归一化评分接口。
 *
 * AnomalyDetector 消费 MonitorInfo，输出各域 signal、0~1 overall_score
 * 以及是否进入诊断/profiling 的布尔建议。它是规则阈值系统，不是机器
 * 学习模型，也不直接 attach eBPF 或推进状态机。
 */

#include <string>
#include <vector>

#include "diagnostics/observability_config.h"

#include "monitor_info.pb.h"

namespace monitor::diagnostics {

/** @brief 异常信号所属的监控域。 */
enum class AnomalyDomain {
  kCpu,
  kMemory,
  kDisk,
  kNetwork,
  kScheduler,
};

/**
 * @brief 一个指标的原始值、归一化分数和触发标记。
 *
 * value 保留指标原单位；score 在 warning/critical 区间归一化到 0~1。
 */
struct AnomalySignal {
  AnomalyDomain domain;
  std::string metric;
  double value = 0.0;
  double score = 0.0;
  bool triggered = false;
};

/**
 * @brief 一轮 MonitorInfo 的异常评估结果。
 *
 * overall_score 取所有信号分数的最大值，避免一个严重单点被其他正常
 * 指标平均稀释；should_diagnose/should_profile 只表达策略建议。
 */
struct AnomalyResult {
  double overall_score = 0.0;
  std::vector<AnomalySignal> signals;
  bool should_diagnose = false;
  bool should_profile = false;
};

/** @brief 基于阈值规则从基础监控指标生成异常信号。 */
class AnomalyDetector {
 public:
  explicit AnomalyDetector(ObservabilityConfig config = {});

  /**
   * @brief 评估一轮监控数据并生成异常分数。
   * @param info Worker 本轮采集的基础 MonitorInfo。
   * @return 各域信号、最大 overall_score 和诊断策略建议。
   */
  AnomalyResult Evaluate(const monitor::proto::MonitorInfo& info) const;

 private:
  ObservabilityConfig config_;
};

}  // namespace monitor::diagnostics
