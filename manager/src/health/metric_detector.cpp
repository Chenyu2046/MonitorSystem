#include "health/metric_detector.h"

#include <algorithm>
#include <cmath>

namespace monitor::health {
namespace {

double Normalize(double value, double warning, double critical) {
  if (!std::isfinite(value) || !std::isfinite(warning) ||
      !std::isfinite(critical) || critical <= warning) {
    return 0.0;
  }
  return std::clamp((value - warning) / (critical - warning), 0.0, 1.0);
}

double DeviationScore(double difference, double spread, double warning,
                      double critical) {
  constexpr double kEpsilon = 1e-12;
  if (!std::isfinite(difference) || !std::isfinite(spread)) return 0.0;
  if (spread <= kEpsilon) return difference <= kEpsilon ? 0.0 : 1.0;
  return Normalize(difference / spread, warning, critical);
}

}  // namespace

DetectorResult MetricDetector::Evaluate(
    double value, std::optional<StaticThreshold> threshold,
    const RollingWindow& history) const {
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
  result.ewma_score = DeviationScore(std::abs(value - ewma), ewma_std,
                                     config_.ewma_warning_sigma,
                                     config_.ewma_critical_sigma);
  if (sample_count < config_.ready_samples) {
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
                                    mad, config_.mad_warning_z,
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
