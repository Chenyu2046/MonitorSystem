#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <string>
#include <unordered_map>

#include "health/health_score_engine.h"
#include "health/metric_detector.h"
#include "health/rolling_window.h"
#include "health/top_signal_codec.h"
#include "rpc/health_score_mapper.h"

namespace {

using Clock = std::chrono::steady_clock;

monitor::proto::MonitorInfo MakeInfo(double cpu_percent = 30.0,
                                     double disk_util = 10.0,
                                     double disk_latency = 1.0) {
  monitor::proto::MonitorInfo info;
  info.set_name("health-host");
  for (int index = 0; index < 4; ++index) {
    auto* cpu = info.add_cpu_stat();
    cpu->set_cpu_name("cpu" + std::to_string(index));
    cpu->set_cpu_percent(cpu_percent);
    cpu->set_io_wait_percent(1.0);
    cpu->set_soft_irq_percent(1.0);
    cpu->set_sample_valid(true);
  }
  info.mutable_cpu_load()->set_load_avg_1(1.0);
  info.mutable_cpu_load()->set_sample_valid(true);
  auto* memory = info.mutable_mem_info();
  memory->set_total(16.0);
  memory->set_avail(8.0);
  memory->set_free(4.0);
  memory->set_used_percent(50.0);
  memory->set_sample_valid(true);
  auto* disk = info.add_disk_info();
  disk->set_name("sda");
  disk->set_util_percent(disk_util);
  disk->set_avg_read_latency_ms(disk_latency);
  disk->set_avg_write_latency_ms(disk_latency);
  disk->set_read_iops(10.0);
  disk->set_write_iops(10.0);
  disk->set_sample_valid(true);
  auto* net = info.add_net_info();
  net->set_name("eth0");
  net->set_rcv_packets_rate(100.0);
  net->set_send_packets_rate(100.0);
  net->set_rcv_rate(100.0);
  net->set_send_rate(200.0);
  net->set_sample_valid(true);
  auto* softirq = info.add_soft_irq();
  softirq->set_cpu("cpu0");
  softirq->set_net_rx(100.0);
  softirq->set_net_tx(100.0);
  softirq->set_sample_valid(true);
  info.mutable_diagnostic()->set_state(
      monitor::proto::OBSERVABILITY_NORMAL);
  return info;
}

monitor::health::HealthResult FeedStable(
    monitor::health::HealthScoreEngine* engine, int count,
    Clock::time_point start = Clock::time_point{}) {
  monitor::health::HealthResult result;
  for (int index = 0; index < count; ++index) {
    result = engine->Evaluate(MakeInfo(), start + std::chrono::seconds(index * 10),
                              80.0);
  }
  return result;
}

void TestRollingWindowEvictsByCountAndAge() {
  monitor::health::RollingWindow count_window(3, std::chrono::seconds(100));
  const auto start = Clock::time_point{};
  assert(count_window.Push(1.0, start));
  assert(count_window.Push(2.0, start + std::chrono::seconds(1)));
  assert(count_window.Push(3.0, start + std::chrono::seconds(2)));
  assert(count_window.Push(4.0, start + std::chrono::seconds(3)));
  assert(count_window.Size() == 3);
  assert(count_window.Median() == 3.0);

  monitor::health::RollingWindow age_window(10, std::chrono::seconds(5));
  assert(age_window.Push(1.0, start));
  assert(age_window.Push(2.0, start + std::chrono::seconds(10)));
  assert(age_window.Size() == 1);
  assert(!age_window.Push(3.0, start + std::chrono::seconds(9)));
  assert(!age_window.Push(std::numeric_limits<double>::infinity(),
                         start + std::chrono::seconds(11)));
}

void TestStableNormalAndColdStart() {
  monitor::health::HealthScoreEngine engine;
  const auto cold = engine.Evaluate(MakeInfo(), Clock::time_point{}, 80.0);
  assert(cold.valid);
  assert(cold.model_state == monitor::health::ModelState::kCold);
  assert(cold.health_score > 90.0);
  const auto ready = FeedStable(&engine, 31, Clock::time_point{} +
                                                 std::chrono::seconds(10));
  assert(ready.valid);
  assert(ready.model_state == monitor::health::ModelState::kReady);
  assert(ready.health_score > 90.0);
  assert(ready.anomaly_rate_5m == 0.0);
}

void TestInstantSpikeAndRelativeHistoryAnomaly() {
  monitor::health::HealthScoreEngine spike_engine;
  const auto start = Clock::time_point{};
  FeedStable(&spike_engine, 31, start);
  const auto spike = spike_engine.Evaluate(
      MakeInfo(95.0), start + std::chrono::seconds(310), 50.0);
  assert(spike.valid);
  assert(spike.cpu_score >= 0.99);
  assert(spike.health_score < 80.0);
  assert(spike.anomaly_rate_5m < 0.10);

  monitor::health::HealthScoreEngine relative_engine;
  FeedStable(&relative_engine, 31, start);
  const auto relative = relative_engine.Evaluate(
      MakeInfo(55.0), start + std::chrono::seconds(310), 70.0);
  assert(relative.cpu_score >= 0.99);
  assert(relative.top_signals.front().detector.threshold_score == 0.0);
  assert(relative.top_signals.front().detector.mad_score >= 0.99);
  assert(relative.top_signals.front().detector.ewma_score >= 0.99);
}

void TestSustainedAndDiskSaturation() {
  const auto start = Clock::time_point{};
  monitor::health::HealthScoreEngine sustained_engine;
  FeedStable(&sustained_engine, 31, start);
  monitor::health::HealthResult saturated;
  for (int index = 0; index < 8; ++index) {
    saturated = sustained_engine.Evaluate(
        MakeInfo(96.0), start + std::chrono::seconds(310 + index * 10), 20.0);
  }
  assert(saturated.health_score < 80.0);
  assert(saturated.anomaly_rate_5m > 0.02);

  monitor::health::HealthScoreEngine disk_engine;
  FeedStable(&disk_engine, 31, start);
  auto disk_info = MakeInfo(30.0, 99.0, 50.0);
  for (auto& cpu : *disk_info.mutable_cpu_stat()) {
    cpu.set_io_wait_percent(30.0);
  }
  const auto disk = disk_engine.Evaluate(
      disk_info, start + std::chrono::seconds(310), 40.0);
  assert(disk.disk_score >= 0.99);
  assert(disk.scheduler_score >= 0.99);
  assert(disk.health_score < 70.0);
  assert(std::string(monitor::health::MetricName(disk.top_signals.front().metric))
             .find("disk") != std::string::npos ||
         disk.top_signals.front().metric == monitor::health::MetricId::kIoWait);
}

void TestHistoricalOnlyIopsAndThroughput() {
  const auto start = Clock::time_point{};
  monitor::health::HealthScoreEngine engine;
  FeedStable(&engine, 31, start);
  auto info = MakeInfo();
  info.mutable_disk_info(0)->set_read_iops(5000.0);
  info.mutable_disk_info(0)->set_write_iops(5000.0);
  info.mutable_net_info(0)->set_rcv_rate(100000.0);
  info.mutable_net_info(0)->set_send_rate(200000.0);
  const auto result = engine.Evaluate(
      info, start + std::chrono::seconds(310), 70.0);
  bool saw_iops = false;
  bool saw_throughput = false;
  for (const auto& signal : result.top_signals) {
    if (signal.metric == monitor::health::MetricId::kDiskIops) {
      saw_iops = true;
      assert(!signal.detector.threshold_available);
      assert(signal.detector.threshold_score == 0.0);
    }
    if (signal.metric == monitor::health::MetricId::kNetworkRxThroughput ||
        signal.metric == monitor::health::MetricId::kNetworkTxThroughput) {
      saw_throughput = true;
      assert(!signal.detector.threshold_available);
    }
  }
  assert(saw_iops);
  assert(saw_throughput);
  assert(result.disk_score >= 0.99);
  assert(result.network_score >= 0.99);
}

void TestMissingDiffersFromZeroAndNonFiniteIsSafe() {
  const auto timestamp = Clock::time_point{};
  monitor::health::HealthScoreEngine zero_engine;
  auto zero = MakeInfo();
  zero.mutable_mem_info()->set_avail(zero.mem_info().total());
  const auto zero_result = zero_engine.Evaluate(zero, timestamp, 100.0);

  monitor::health::HealthScoreEngine missing_engine;
  auto missing = MakeInfo();
  missing.clear_mem_info();
  const auto missing_result = missing_engine.Evaluate(missing, timestamp, 100.0);
  assert(zero_result.valid && missing_result.valid);
  assert(zero_result.confidence > missing_result.confidence);
  assert(zero_result.memory_score == 0.0);

  monitor::health::HealthScoreEngine absent_engine;
  auto absent = MakeInfo();
  for (auto& cpu : *absent.mutable_cpu_stat()) cpu.set_sample_valid(false);
  absent.mutable_cpu_load()->set_sample_valid(false);
  absent.mutable_mem_info()->set_sample_valid(false);
  absent.mutable_disk_info(0)->set_sample_valid(false);
  absent.mutable_net_info(0)->set_sample_valid(false);
  const auto absent_result = absent_engine.Evaluate(absent, timestamp, 100.0);
  assert(absent_result.valid);
  assert(absent_result.confidence < zero_result.confidence);

  monitor::health::HealthScoreEngine finite_engine;
  auto invalid = MakeInfo();
  invalid.mutable_cpu_stat(0)->set_cpu_percent(
      std::numeric_limits<float>::quiet_NaN());
  const auto finite = finite_engine.Evaluate(invalid, timestamp, 90.0);
  assert(finite.valid);
  assert(std::isfinite(finite.health_score));
  const auto out_of_order = finite_engine.Evaluate(
      invalid, timestamp - std::chrono::seconds(1), 90.0);
  assert(!out_of_order.valid);
  assert(std::isfinite(out_of_order.health_score));
}

void TestNarWindowAndConfigurationValidation() {
  monitor::health::HealthConfig config;
  config.nar_window = std::chrono::seconds(30);
  monitor::health::HealthScoreEngine engine(config);
  const auto start = Clock::time_point{};
  FeedStable(&engine, 31, start);
  for (int index = 0; index < 4; ++index) {
    engine.Evaluate(MakeInfo(95.0),
                    start + std::chrono::seconds(310 + index * 10), 30.0);
  }
  const auto current = engine.Evaluate(
      MakeInfo(95.0), start + std::chrono::seconds(350), 30.0);
  assert(current.anomaly_rate_5m > 0.10);

  const char* previous = std::getenv("EWMA_ALPHA");
  const bool was_set = previous != nullptr;
  const std::string previous_value = previous ? previous : "";
  assert(setenv("EWMA_ALPHA", "invalid", 1) == 0);
  monitor::health::HealthConfig parsed;
  std::string error;
  assert(!monitor::health::LoadHealthConfigFromEnvironment(&parsed, &error));
  assert(!error.empty());
  if (was_set) {
    assert(setenv("EWMA_ALPHA", previous_value.c_str(), 1) == 0);
  } else {
    unsetenv("EWMA_ALPHA");
  }
}

void TestQueryProtoMapping() {
  monitor::PerformanceRecord input;
  input.resource_score = 62.0F;
  input.health_score = 91.0F;
  input.anomaly_score = 0.09F;
  input.anomaly_rate_5m = 0.043F;
  input.confidence = 1.0F;
  input.state = "NORMAL";
  input.model_state = "READY";
  input.health_valid = true;
  monitor::health::TopSignal top;
  top.metric = monitor::health::MetricId::kDiskIops;
  top.value = 1000.0;
  top.detector.threshold_available = false;
  top.detector.mad_score = 1.0;
  top.detector.ewma_score = 1.0;
  top.detector.anomaly_votes = 2;
  top.detector.anomaly_score = 1.0;
  top.detector.anomalous = true;
  top.detector.model_state = monitor::health::ModelState::kReady;
  input.top_signals.push_back(top);
  monitor::proto::PerformanceRecord output;
  monitor::PopulateHealthScoreFields(input, &output);
  assert(output.health_valid());
  assert(output.health_score() == 91.0F);
  assert(output.resource_score() == 62.0F);
  assert(output.state() == "NORMAL");
  assert(output.model_state() == "READY");
  assert(output.top_signals_size() == 1);
  assert(output.top_signals(0).metric() == "disk_iops");
  assert(!output.top_signals(0).threshold_available());

  const auto encoded = monitor::health::EncodeTopSignals(input.top_signals);
  const auto decoded = monitor::health::DecodeTopSignals(encoded);
  assert(decoded.size() == 1);
  assert(decoded.front().metric == monitor::health::MetricId::kDiskIops);
  assert(decoded.front().detector.anomaly_votes == 2);

  monitor::ServerScoreSummary summary;
  summary.resource_score = 60.0F;
  monitor::proto::ServerScoreSummary summary_output;
  monitor::PopulateHealthScoreFields(summary, &summary_output);
  assert(!summary_output.health_valid());
  assert(summary_output.resource_score() == 60.0F);
  assert(summary_output.health_score() == 0.0F);
}

void TestLowTrafficStaleStateCleanup() {
  std::unordered_map<std::string, monitor::health::HealthScoreEngine> engines;
  auto [it, inserted] = engines.try_emplace("stale-host");
  assert(inserted);
  const auto start = Clock::time_point{};
  assert(it->second.Evaluate(MakeInfo(), start, 90.0).valid);
  const auto removed = monitor::health::PruneStaleHealthEngines(
      &engines, start + std::chrono::minutes(6), std::chrono::minutes(5));
  assert(removed.size() == 1 && removed.front() == "stale-host");
  assert(engines.empty());
}

}  // namespace

int main() {
  TestRollingWindowEvictsByCountAndAge();
  TestStableNormalAndColdStart();
  TestInstantSpikeAndRelativeHistoryAnomaly();
  TestSustainedAndDiskSaturation();
  TestHistoricalOnlyIopsAndThroughput();
  TestMissingDiffersFromZeroAndNonFiniteIsSafe();
  TestNarWindowAndConfigurationValidation();
  TestQueryProtoMapping();
  TestLowTrafficStaleStateCleanup();
  return 0;
}
