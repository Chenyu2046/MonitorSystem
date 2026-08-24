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
#include "rpc/query_rate_mapper.h"
#include "rpc/softirq_detail_mapper.h"

namespace {

using Clock = std::chrono::system_clock;

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

void TestNetworkSoftIrqUsesPerCpuMaximum() {
  monitor::health::HealthScoreEngine engine;
  auto info = MakeInfo();
  info.mutable_soft_irq(0)->set_net_rx(60000.0);
  info.mutable_soft_irq(0)->set_net_tx(0.0);
  auto* second_cpu = info.add_soft_irq();
  second_cpu->set_cpu("cpu1");
  second_cpu->set_net_rx(60000.0);
  second_cpu->set_net_tx(0.0);
  second_cpu->set_sample_valid(true);

  const auto result = engine.Evaluate(info, Clock::time_point{}, 90.0);
  bool found = false;
  for (const auto& signal : result.top_signals) {
    if (signal.metric == monitor::health::MetricId::kNetworkSoftIrqPerSec) {
      assert(signal.value == 60000.0);
      found = true;
    }
  }
  assert(found);
}

void TestZeroVarianceUsesRelativeSpreadFloor() {
  const auto start = Clock::time_point{};
  monitor::health::HealthScoreEngine engine;
  for (int index = 0; index < 31; ++index) {
    auto info = MakeInfo();
    info.mutable_disk_info(0)->set_read_iops(50.0);
    info.mutable_disk_info(0)->set_write_iops(50.0);
    assert(engine.Evaluate(info, start + std::chrono::seconds(index * 10),
                           90.0)
               .valid);
  }
  auto jitter = MakeInfo();
  jitter.mutable_disk_info(0)->set_read_iops(50.5);
  jitter.mutable_disk_info(0)->set_write_iops(50.5);
  const auto small = engine.Evaluate(
      jitter, start + std::chrono::seconds(310), 90.0);
  double small_score = 0.0;
  for (const auto& signal : small.top_signals) {
    if (signal.metric == monitor::health::MetricId::kDiskIops) {
      small_score = signal.detector.anomaly_score;
    }
  }
  assert(small_score < 1.0);

  auto spike = MakeInfo();
  spike.mutable_disk_info(0)->set_read_iops(500.0);
  spike.mutable_disk_info(0)->set_write_iops(500.0);
  const auto large = engine.Evaluate(
      spike, start + std::chrono::seconds(320), 90.0);
  assert(large.disk_score >= 0.5);
}

void TestMinimumHistoryDuration() {
  const auto start = Clock::time_point{};
  monitor::health::HealthScoreEngine fast_engine;
  auto fast = FeedStable(&fast_engine, 30, start);
  (void)fast;
  // 30 one-second samples satisfy count but not the configured 5-minute
  // history duration, so the model must remain warming.
  monitor::health::HealthScoreEngine one_second_engine;
  monitor::health::HealthResult result;
  for (int index = 0; index < 30; ++index) {
    result = one_second_engine.Evaluate(
        MakeInfo(), start + std::chrono::seconds(index), 90.0);
  }
  assert(result.model_state == monitor::health::ModelState::kWarming);
}

void TestEventTimeControlsCounterRate() {
  const auto start = Clock::time_point{};
  monitor::health::HealthScoreEngine engine;
  auto first = MakeInfo();
  first.mutable_net_info(0)->set_drop_in(100);
  first.mutable_net_info(0)->set_err_in(50);
  engine.Evaluate(first, start, 90.0);
  auto delayed = MakeInfo();
  delayed.mutable_net_info(0)->set_drop_in(200);
  delayed.mutable_net_info(0)->set_err_in(70);
  const auto result = engine.Evaluate(
      delayed, start + std::chrono::seconds(10), 90.0);
  bool saw_drop_rate = false;
  bool saw_error_rate = false;
  for (const auto& signal : result.top_signals) {
    if (signal.metric == monitor::health::MetricId::kNetworkDropsPerSec) {
      saw_drop_rate = signal.value == 10.0;
    }
    if (signal.metric == monitor::health::MetricId::kNetworkErrorsPerSec) {
      saw_error_rate = signal.value == 2.0;
    }
  }
  assert(saw_drop_rate && saw_error_rate);
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
  assert(out_of_order.valid);
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

  input.cpu_percent_rate = 0.11F;
  input.usr_percent_rate = 0.22F;
  input.system_percent_rate = 0.33F;
  input.io_wait_percent_rate = 0.44F;
  input.load_avg_1_rate = 0.55F;
  input.load_avg_3_rate = 0.66F;
  input.load_avg_15_rate = 0.77F;
  input.mem_used_percent_rate = 0.88F;
  input.disk_util_percent_rate = 0.99F;
  input.send_rate_rate = 1.11F;
  input.rcv_rate_rate = 1.22F;
  monitor::proto::PerformanceRecord rate_output;
  monitor::PopulatePerformanceRateFields(input, &rate_output);
  assert(rate_output.cpu_percent_rate() == 0.11F);
  assert(rate_output.usr_percent_rate() == 0.22F);
  assert(rate_output.system_percent_rate() == 0.33F);
  assert(rate_output.io_wait_percent_rate() == 0.44F);
  assert(rate_output.load_avg_1_rate() == 0.55F);
  assert(rate_output.load_avg_3_rate() == 0.66F);
  assert(rate_output.load_avg_15_rate() == 0.77F);
  assert(rate_output.mem_used_percent_rate() == 0.88F);
  assert(rate_output.disk_util_percent_rate() == 0.99F);
  assert(rate_output.send_rate_rate() == 1.11F);
  assert(rate_output.rcv_rate_rate() == 1.22F);

  monitor::NetDetailRecord net;
  net.rcv_bytes_rate_rate = 0.11F;
  net.snd_bytes_rate_rate = 0.22F;
  net.err_in_rate = 0.33F;
  net.err_out_rate = 0.44F;
  net.drop_in_rate = 0.55F;
  net.drop_out_rate = 0.66F;
  monitor::proto::NetDetailRecord net_output;
  monitor::PopulateNetRateFields(net, &net_output);
  assert(net_output.rcv_bytes_rate_rate() == 0.11F);
  assert(net_output.snd_bytes_rate_rate() == 0.22F);
  assert(net_output.err_in_rate() == 0.33F);
  assert(net_output.err_out_rate() == 0.44F);
  assert(net_output.drop_in_rate() == 0.55F);
  assert(net_output.drop_out_rate() == 0.66F);

  monitor::DiskDetailRecord disk;
  disk.read_bytes_per_sec_rate = 0.11F;
  disk.write_bytes_per_sec_rate = 0.22F;
  disk.read_iops_rate = 0.33F;
  disk.write_iops_rate = 0.44F;
  disk.util_percent_rate = 0.55F;
  monitor::proto::DiskDetailRecord disk_output;
  monitor::PopulateDiskRateFields(disk, &disk_output);
  assert(disk_output.read_bytes_per_sec_rate() == 0.11F);
  assert(disk_output.write_bytes_per_sec_rate() == 0.22F);
  assert(disk_output.read_iops_rate() == 0.33F);
  assert(disk_output.write_iops_rate() == 0.44F);
  assert(disk_output.util_percent_rate() == 0.55F);

  monitor::MemDetailRecord memory;
  memory.total_rate = 0.11F;
  memory.free_rate = 0.22F;
  memory.avail_rate = 0.33F;
  memory.active_rate = 0.44F;
  memory.inactive_rate = 0.55F;
  monitor::proto::MemDetailRecord memory_output;
  monitor::PopulateMemRateFields(memory, &memory_output);
  assert(memory_output.total_rate() == 0.11F);
  assert(memory_output.free_rate() == 0.22F);
  assert(memory_output.avail_rate() == 0.33F);
  assert(memory_output.active_rate() == 0.44F);
  assert(memory_output.inactive_rate() == 0.55F);

  monitor::SoftIrqDetailRecord softirq;
  softirq.hi = 0.11F;
  softirq.timer = 0.22F;
  softirq.net_tx = 0.33F;
  softirq.net_rx = 0.44F;
  softirq.block = 0.55F;
  softirq.irq_poll = 0.66F;
  softirq.tasklet = 0.77F;
  softirq.sched = 0.88F;
  softirq.hrtimer = 0.99F;
  softirq.rcu = 1.11F;
  monitor::proto::SoftIrqDetailRecord softirq_output;
  monitor::PopulateSoftIrqRateFields(softirq, &softirq_output);
  assert(softirq_output.hi_per_sec() == 0.11F);
  assert(softirq_output.timer_per_sec() == 0.22F);
  assert(softirq_output.net_tx_per_sec() == 0.33F);
  assert(softirq_output.net_rx_per_sec() == 0.44F);
  assert(softirq_output.block_per_sec() == 0.55F);
  assert(softirq_output.irq_poll_per_sec() == 0.66F);
  assert(softirq_output.tasklet_per_sec() == 0.77F);
  assert(softirq_output.sched_per_sec() == 0.88F);
  assert(softirq_output.hrtimer_per_sec() == 0.99F);
  assert(softirq_output.rcu_per_sec() == 1.11F);
  assert(softirq_output.hi() == 0);
}

void TestLowTrafficStaleStateCleanup() {
  std::unordered_map<std::string, monitor::health::HealthScoreEngine> engines;
  auto [it, inserted] = engines.try_emplace("stale-host");
  assert(inserted);
  const auto start = Clock::time_point{};
  const auto activity_start =
      monitor::health::HealthScoreEngine::ActivityClock::now();
  assert(it->second.Evaluate(MakeInfo(), start, 90.0, activity_start).valid);
  const auto removed = monitor::health::PruneStaleHealthEngines(
      &engines, activity_start + std::chrono::minutes(6),
      std::chrono::minutes(5));
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
  TestNetworkSoftIrqUsesPerCpuMaximum();
  TestZeroVarianceUsesRelativeSpreadFloor();
  TestMinimumHistoryDuration();
  TestEventTimeControlsCounterRate();
  TestMissingDiffersFromZeroAndNonFiniteIsSafe();
  TestNarWindowAndConfigurationValidation();
  TestQueryProtoMapping();
  TestLowTrafficStaleStateCleanup();
  return 0;
}
