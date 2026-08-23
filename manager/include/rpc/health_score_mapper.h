#pragma once

#include "query_api.pb.h"
#include "query_manager.h"

namespace monitor {

template <typename ProtoRecord>
inline void PopulateTopSignals(const std::vector<health::TopSignal>& signals,
                               ProtoRecord* output) {
  for (const auto& signal : signals) {
    auto* target = output->add_top_signals();
    target->set_metric(health::MetricName(signal.metric));
    target->set_value(signal.value);
    target->set_threshold_available(signal.detector.threshold_available);
    target->set_threshold_score(signal.detector.threshold_score);
    target->set_mad_score(signal.detector.mad_score);
    target->set_ewma_score(signal.detector.ewma_score);
    target->set_anomaly_votes(signal.detector.anomaly_votes);
    target->set_anomaly_score(signal.detector.anomaly_score);
    target->set_anomalous(signal.detector.anomalous);
    target->set_model_state(
        health::ModelStateName(signal.detector.model_state));
  }
}

inline void PopulateHealthScoreFields(
    const PerformanceRecord& input,
    monitor::proto::PerformanceRecord* output) {
  if (!output) return;
  output->set_resource_score(input.resource_score);
  output->set_health_valid(input.health_valid);
  if (!input.health_valid) return;
  output->set_health_score(input.health_score);
  output->set_anomaly_score(input.anomaly_score);
  output->set_anomaly_rate_5m(input.anomaly_rate_5m);
  output->set_confidence(input.confidence);
  output->set_state(input.state);
  output->set_model_state(input.model_state);
  PopulateTopSignals(input.top_signals, output);
}

inline void PopulateHealthScoreFields(
    const ServerScoreSummary& input,
    monitor::proto::ServerScoreSummary* output) {
  if (!output) return;
  output->set_resource_score(input.resource_score);
  output->set_health_valid(input.health_valid);
  if (!input.health_valid) return;
  output->set_health_score(input.health_score);
  output->set_anomaly_score(input.anomaly_score);
  output->set_anomaly_rate_5m(input.anomaly_rate_5m);
  output->set_confidence(input.confidence);
  output->set_state(input.state);
  output->set_model_state(input.model_state);
  PopulateTopSignals(input.top_signals, output);
}

}  // namespace monitor
