#include "health/metric_detector.h"

#include <algorithm>
#include <cmath>

namespace monitor::health {
namespace {

double Normalize(double value, double warning, double critical) {
  // 将告警区间线性映射到 [0, 1]，并截断区间外的结果。
  if (!std::isfinite(value) || !std::isfinite(warning) ||
      !std::isfinite(critical) || critical <= warning) {
    return 0.0;
  }
  return std::clamp((value - warning) / (critical - warning), 0.0, 1.0);
}

double DeviationScore(double difference, double spread, double baseline,
                      double warning, double critical) {
  // 用相对基线的有效波动范围归一化偏差，避免低方差时分母失真。
  constexpr double kEpsilon = 1e-12;
  if (!std::isfinite(difference) || !std::isfinite(spread)) return 0.0;
  const double relative_floor = std::abs(baseline) * 0.01;
  const double effective_spread = std::max(
      spread, std::max(1e-6, relative_floor));
  if (effective_spread <= kEpsilon) return 0.0;
  return Normalize(difference / effective_spread, warning, critical);
}

}  // namespace

DetectorResult MetricDetector::Evaluate(
    double value, std::optional<StaticThreshold> threshold,
    const RollingWindow& history,
    RollingWindow::Clock::time_point timestamp) const {
  // 综合静态阈值、EWMA 和成熟窗口上的 MAD 结果，使用投票确认异常。
  DetectorResult result;
  if (!std::isfinite(value)) return result;

  result.threshold_available = threshold.has_value();
  if (threshold) {
    result.threshold_score =
        Normalize(value, threshold->warning, threshold->critical);
  }
  const std::size_t sample_count = history.Size() + 1;
  if (sample_count < 10) {
    result.model_state = ModelState::kCold;
    result.anomaly_score = result.threshold_score;
    result.anomalous = result.threshold_score >= 0.5;
    result.anomaly_votes =
        result.threshold_available && result.anomalous ? 1 : 0;
    return result;
  }

  const auto [ewma, ewma_std] = history.EwmaMeanStd(config_.ewma_alpha);
  result.ewma_score = DeviationScore(std::abs(value - ewma), ewma_std, ewma,
                                     config_.ewma_warning_sigma,
                                     config_.ewma_critical_sigma);
  if (sample_count < config_.ready_samples ||
      history.Age(timestamp) < config_.minimum_history_duration) {
    result.model_state = ModelState::kWarming;
    result.ewma_score *= 0.5;
    result.anomaly_votes =
                           (result.threshold_available &&
                                    result.threshold_score >= 0.5
                                ? 1
                                : 0) +
                           (result.ewma_score >= 0.5 ? 1 : 0);
    result.anomalous = result.anomaly_votes >= 2;
    const double strongest = std::max(result.threshold_score, result.ewma_score);
    result.anomaly_score = result.anomalous ? strongest : strongest * 0.25;
    return result;
  }

  result.model_state = ModelState::kReady;
  const double mad = history.Mad();
  result.mad_score = DeviationScore(0.6745 * std::abs(value - history.Median()),
                                    mad, history.Median(), config_.mad_warning_z,
                                    config_.mad_critical_z);
  result.anomaly_votes =
                         (result.threshold_available &&
                                  result.threshold_score >= 0.5
                              ? 1
                              : 0) +
                         (result.mad_score >= 0.5 ? 1 : 0) +
                         (result.ewma_score >= 0.5 ? 1 : 0);
  result.anomalous = result.anomaly_votes >= config_.consensus_min_votes;
  const double strongest = std::max(
      result.threshold_score, std::max(result.mad_score, result.ewma_score));
  result.anomaly_score = result.anomalous ? strongest : strongest * 0.25;
  result.anomaly_score = std::clamp(result.anomaly_score, 0.0, 1.0);
  return result;
}

}  // namespace monitor::health
