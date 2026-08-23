/**
 * @file cpu_overview_test.cpp
 * @brief 验证 Manager 逐核 CPU 概览、peak core、非有限值过滤和评分输入。
 *
 * 测试重点是普通概览取有效核平均值，而异常/辅助语义仍保留最忙核；
 * 这些测试防止回归到只读取单核或把 NaN/Inf 带入整机 score。
 */

#include <cassert>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <limits>
#include <functional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "host_manager.h"
#include "canonical_host_key.h"
#include "diagnostics/anomaly_detector.h"
#include "diagnostics/remote_health_feedback.h"
#include "diagnostics/observability_state.h"
#include "rpc/softirq_detail_mapper.h"

namespace {

/** @brief 构造指定核数和 CPU 值的测试 MonitorInfo。 */
monitor::proto::MonitorInfo MakeInfo(const char* name, int cpu_count,
                                     float cpu_percent, float load_avg_1 = 0) {
  monitor::proto::MonitorInfo info;
  info.set_name(name);
  info.mutable_cpu_load()->set_load_avg_1(load_avg_1);
  info.mutable_cpu_load()->set_load_avg_3(0.0F);
  info.mutable_cpu_load()->set_load_avg_15(0.0F);
  info.mutable_cpu_load()->set_sample_valid(true);
  info.mutable_mem_info()->set_total(16.0F);
  info.mutable_mem_info()->set_free(16.0F);
  info.mutable_mem_info()->set_avail(16.0F);
  info.mutable_mem_info()->set_used_percent(0.0F);
  info.mutable_mem_info()->set_sample_valid(true);
  auto* net = info.add_net_info();
  net->set_name("eth0");
  net->set_sample_valid(true);
  auto* disk = info.add_disk_info();
  disk->set_name("sda");
  disk->set_sample_valid(true);
  for (int index = 0; index < cpu_count; ++index) {
    auto* cpu = info.add_cpu_stat();
    cpu->set_cpu_name("cpu" + std::to_string(index));
    cpu->set_cpu_percent(cpu_percent);
    cpu->set_sample_valid(true);
  }
  return info;
}

std::int64_t NowUnixMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

void SetSampleIdentity(monitor::proto::MonitorInfo* info,
                       std::uint64_t sequence) {
  info->set_sample_sequence(sequence);
  info->set_sample_timestamp_ms(NowUnixMs());
}

monitor::HostScore WaitForScore(monitor::HostManager* manager,
                                const char* host_name) {
  for (int attempt = 0; attempt < 100; ++attempt) {
    const auto scores = manager->GetAllHostScores();
    const auto it = scores.find(host_name);
    if (it != scores.end()) return it->second;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  assert(false);
  return {};
}

/** @brief 验证所有有效 CPU 核参与普通平均值和 peak core。 */
void TestCpuOverviewAveragesAllCores() {
  monitor::proto::MonitorInfo info;
  auto* cpu0 = info.add_cpu_stat();
  cpu0->set_cpu_name("cpu0");
  cpu0->set_cpu_percent(20.0);
  cpu0->set_usr_percent(20.0);
  cpu0->set_system_percent(10.0);
  auto* cpu1 = info.add_cpu_stat();
  cpu1->set_cpu_name("cpu1");
  cpu1->set_cpu_percent(25.0);
  cpu1->set_usr_percent(60.0);
  cpu1->set_system_percent(20.0);
  cpu1->set_io_wait_percent(10.0);
  auto* cpu2 = info.add_cpu_stat();
  cpu2->set_cpu_name("cpu2");
  cpu2->set_cpu_percent(95.0);
  auto* cpu3 = info.add_cpu_stat();
  cpu3->set_cpu_name("cpu3");
  cpu3->set_cpu_percent(20.0);

  const auto overview = monitor::BuildCpuOverview(info);
  assert(overview.cpu_count == 4);
  assert(overview.cpu_percent == 40.0F);
  assert(overview.usr_percent == 20.0F);
  assert(overview.system_percent == 7.5F);
  assert(overview.io_wait_percent == 2.5F);
  assert(overview.peak_core_name == "cpu2");
  assert(overview.peak_cpu_percent == 95.0F);

  const auto empty = monitor::BuildCpuOverview(monitor::proto::MonitorInfo{});
  assert(empty.cpu_count == 0);
  assert(empty.cpu_percent == 0.0F);
}

/** @brief 验证 HostManager score 使用普通 CPU 概览值。 */
void TestCalcScoreUsesAverageCpu() {
  monitor::HostManager manager;
  manager.Start();

  auto info = MakeInfo("average-host", 2, 10.0F);
  info.mutable_cpu_stat(1)->set_cpu_percent(90.0F);
  assert(manager.Submit(info) == monitor::DataReceiveResult::kAccepted);
  const auto host_score = WaitForScore(&manager, "average-host");
  manager.Stop();

  assert(std::isfinite(host_score.score));
  assert(std::abs(host_score.score - 82.5) < 0.01);
  assert(host_score.health.valid);
  assert(host_score.health.resource_score == host_score.score);
  assert(host_score.health.model_state == monitor::health::ModelState::kCold);
  assert(manager.GetBestHost() == "average-host");
  assert(manager.GetHealthiestHost() == "average-host");
}

/** @brief 验证 CPU 核数仍参与 load score，不被错误扣减。 */
void TestCpuCountIsNotReducedForLoadScore() {
  monitor::HostManager manager;
  manager.Start();

  const auto info = MakeInfo("eight-core-host", 8, 0.0F, 10.0F);
  assert(manager.Submit(info) == monitor::DataReceiveResult::kAccepted);
  const auto score = WaitForScore(&manager, "eight-core-host").score;
  manager.Stop();

  assert(std::isfinite(score));
  assert(std::abs(score - 87.5) < 0.01);
}

/** @brief 验证 NaN/Inf 样本不会污染平均值或核计数。 */
void TestCpuOverviewFiltersNonFiniteSamples() {
  monitor::proto::MonitorInfo info;
  auto* cpu0 = info.add_cpu_stat();
  cpu0->set_cpu_name("cpu0");
  cpu0->set_cpu_percent(20.0F);
  auto* invalid = info.add_cpu_stat();
  invalid->set_cpu_name("cpu1");
  invalid->set_cpu_percent(std::numeric_limits<float>::quiet_NaN());
  auto* cpu2 = info.add_cpu_stat();
  cpu2->set_cpu_name("cpu2");
  cpu2->set_cpu_percent(80.0F);

  const auto overview = monitor::BuildCpuOverview(info);
  assert(overview.cpu_count == 2);
  assert(overview.cpu_percent == 50.0F);
  assert(overview.peak_core_name == "cpu2");
  assert(overview.peak_cpu_percent == 80.0F);

  monitor::proto::MonitorInfo all_invalid;
  auto* nan_cpu = all_invalid.add_cpu_stat();
  nan_cpu->set_cpu_percent(std::numeric_limits<float>::quiet_NaN());
  auto* inf_cpu = all_invalid.add_cpu_stat();
  inf_cpu->set_cpu_percent(std::numeric_limits<float>::infinity());
  const auto empty = monitor::BuildCpuOverview(all_invalid);
  assert(empty.cpu_count == 0);
  assert(empty.cpu_percent == 0.0F);
  assert(empty.peak_core_name.empty());
  assert(empty.peak_cpu_percent == 0.0F);
}

/** @brief 验证全是无效 CPU 样本时 score 仍保持有限。 */
void TestAllInvalidCpuScoreIsFinite() {
  monitor::HostManager manager;
  manager.Start();

  auto info = MakeInfo("invalid-cpu-host", 2, 0.0F);
  info.mutable_cpu_stat(0)->set_cpu_percent(
      std::numeric_limits<float>::quiet_NaN());
  info.mutable_cpu_stat(1)->set_cpu_percent(
      std::numeric_limits<float>::infinity());
  assert(manager.Submit(info) == monitor::DataReceiveResult::kAccepted);
  const auto score = WaitForScore(&manager, "invalid-cpu-host").score;
  assert(manager.GetBestHost().empty());
  manager.Stop();

  assert(std::isfinite(score));
}

monitor::HostScore WaitForScoreWhere(
    monitor::HostManager* manager, const char* host_name,
    const std::function<bool(const monitor::HostScore&)>& predicate) {
  for (int attempt = 0; attempt < 300; ++attempt) {
    const auto scores = manager->GetAllHostScores();
    const auto it = scores.find(host_name);
    if (it != scores.end() && predicate(it->second)) return it->second;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  assert(false);
  return {};
}

void TestNegativeMemoryIsNotSchedulable() {
  monitor::HostManager manager;
  assert(manager.Start());
  auto info = MakeInfo("negative-memory-host", 1, 0.0F);
  info.mutable_mem_info()->set_free(-1.0F);
  assert(manager.Submit(info) == monitor::DataReceiveResult::kAccepted);
  const auto score = WaitForScore(&manager, "negative-memory-host");
  assert(!score.score_valid);
  assert(manager.GetBestHost().empty());
  manager.Stop();
}

void TestNetworkOverviewUsesAllInterfaces() {
  monitor::proto::MonitorInfo info;
  auto* first = info.add_net_info();
  first->set_name("eth0");
  first->set_rcv_rate(1.0F);
  first->set_send_rate(2.0F);
  auto* second = info.add_net_info();
  second->set_name("eth1");
  second->set_rcv_rate(100.0F);
  second->set_send_rate(200.0F);
  const auto overview = monitor::BuildNetworkOverview(info);
  assert(overview.interface_count == 2);
  assert(overview.reported_total_recv_kib_per_sec == 101.0);
  assert(overview.reported_total_send_kib_per_sec == 202.0);
  assert(overview.peak_recv_kib_per_sec == 100.0);
  assert(overview.peak_send_kib_per_sec == 200.0);
}

void TestManagerSizeConfigIsFailFast() {
  assert(monitor::ParsePositiveSizeConfig(nullptr, 8).value() == 8);
  assert(monitor::ParsePositiveSizeConfig("1", 8).value() == 1);
  assert(!monitor::ParsePositiveSizeConfig("", 8));
  assert(!monitor::ParsePositiveSizeConfig("0", 8));
  assert(!monitor::ParsePositiveSizeConfig("abc", 8));
  assert(!monitor::ParsePositiveSizeConfig("12tail", 8));
  assert(!monitor::ParsePositiveSizeConfig("184467440737095516160", 8));
}

class ScopedSizeConfigEnvironment {
 public:
  ScopedSizeConfigEnvironment() {
    for (const char* name : kNames) {
      const char* value = std::getenv(name);
      values_.emplace_back(name, value ? value : "");
      was_set_.push_back(value != nullptr);
      assert(setenv(name, "1", 1) == 0);
    }
  }

  ~ScopedSizeConfigEnvironment() {
    for (std::size_t index = 0; index < kNames.size(); ++index) {
      if (was_set_[index]) {
        setenv(kNames[index], values_[index].second.c_str(), 1);
      } else {
        unsetenv(kNames[index]);
      }
    }
  }

  static constexpr std::array<const char*, 5> kNames = {
      "KERNSCOPE_MANAGER_SHARDS", "KERNSCOPE_SHARD_QUEUE_CAPACITY",
      "KERNSCOPE_SHARD_QUEUE_MAX_BYTES", "KERNSCOPE_PERSIST_QUEUE_CAPACITY",
      "KERNSCOPE_PERSIST_QUEUE_MAX_BYTES"};

 private:
  std::vector<std::pair<std::string, std::string>> values_;
  std::vector<bool> was_set_;
};

void TestManagerStartRejectsEveryInvalidSizeEnvironment() {
  ScopedSizeConfigEnvironment environment;
  for (const char* name : ScopedSizeConfigEnvironment::kNames) {
    assert(setenv(name, "invalid", 1) == 0);
    monitor::HostManager manager;
    const bool started = manager.Start();
    if (started) manager.Stop();
    assert(!started);
    assert(setenv(name, "1", 1) == 0);
  }
}

void TestManagerStartRejectsInvalidHealthEnvironment() {
  const char* previous = std::getenv("MAD_WARNING_Z");
  const bool was_set = previous != nullptr;
  const std::string previous_value = previous ? previous : "";
  assert(setenv("MAD_WARNING_Z", "invalid", 1) == 0);
  monitor::HostManager manager;
  assert(!manager.Start());
  if (was_set) {
    assert(setenv("MAD_WARNING_Z", previous_value.c_str(), 1) == 0);
  } else {
    unsetenv("MAD_WARNING_Z");
  }
}

void TestSoftIrqQueryUsesOnlyPerSecondFields() {
  monitor::SoftIrqDetailRecord input;
  input.hi = 1.0F;
  input.timer = 2.0F;
  input.net_tx = 3.0F;
  input.net_rx = 4.0F;
  input.block = 5.0F;
  input.irq_poll = 6.0F;
  input.tasklet = 7.0F;
  input.sched = 8.0F;
  input.hrtimer = 9.0F;
  input.rcu = 10.0F;

  monitor::proto::SoftIrqDetailRecord output;
  monitor::PopulateSoftIrqRateFields(input, &output);

  const auto* descriptor = output.GetDescriptor();
  const auto* reflection = output.GetReflection();
  for (int tag = 10; tag <= 19; ++tag) {
    assert(!reflection->HasField(output, descriptor->FindFieldByNumber(tag)));
  }
  assert(output.hi_per_sec() == 1.0F && output.timer_per_sec() == 2.0F);
  assert(output.net_tx_per_sec() == 3.0F && output.net_rx_per_sec() == 4.0F);
  assert(output.block_per_sec() == 5.0F &&
         output.irq_poll_per_sec() == 6.0F);
  assert(output.tasklet_per_sec() == 7.0F && output.sched_per_sec() == 8.0F);
  assert(output.hrtimer_per_sec() == 9.0F && output.rcu_per_sec() == 10.0F);
}

void TestHostManagerSpikeRecovery() {
  monitor::HostManager manager;
  assert(manager.Start());
  for (int index = 0; index < 31; ++index) {
    auto stable = MakeInfo("spike-recovery-host", 4, 30.0F, 1.0F);
    stable.mutable_diagnostic()->set_state(
        monitor::proto::OBSERVABILITY_NORMAL);
    assert(manager.Submit(stable) == monitor::DataReceiveResult::kAccepted);
  }
  WaitForScoreWhere(&manager, "spike-recovery-host", [](const auto& score) {
    return score.health.model_state == monitor::health::ModelState::kReady;
  });

  auto spike = MakeInfo("spike-recovery-host", 4, 95.0F, 1.0F);
  spike.mutable_diagnostic()->set_state(
      monitor::proto::OBSERVABILITY_SUSPECT);
  assert(manager.Submit(spike) == monitor::DataReceiveResult::kAccepted);
  const auto spike_score = WaitForScoreWhere(
      &manager, "spike-recovery-host",
      [](const auto& score) { return score.health.health_score < 80.0; });

  auto recovery = MakeInfo("spike-recovery-host", 4, 30.0F, 1.0F);
  recovery.mutable_diagnostic()->set_state(
      monitor::proto::OBSERVABILITY_NORMAL);
  assert(manager.Submit(recovery) == monitor::DataReceiveResult::kAccepted);
  const auto recovery_score = WaitForScoreWhere(
      &manager, "spike-recovery-host", [&](const auto& score) {
        return score.health.health_score > spike_score.health.health_score;
      });
  assert(recovery_score.health.health_score > 90.0);
  manager.Stop();
}

void TestDiskIncidentIncludesHealthTopSignals() {
  monitor::HostManager manager;
  assert(manager.Start());
  for (int index = 0; index < 31; ++index) {
    auto stable = MakeInfo("disk-incident-host", 4, 30.0F, 1.0F);
    stable.mutable_diagnostic()->set_state(
        monitor::proto::OBSERVABILITY_NORMAL);
    assert(manager.Submit(stable) == monitor::DataReceiveResult::kAccepted);
  }
  WaitForScoreWhere(&manager, "disk-incident-host", [](const auto& score) {
    return score.health.model_state == monitor::health::ModelState::kReady;
  });

  auto saturated = MakeInfo("disk-incident-host", 4, 30.0F, 1.0F);
  for (auto& cpu : *saturated.mutable_cpu_stat()) {
    cpu.set_io_wait_percent(9.0F);
  }
  saturated.mutable_disk_info(0)->set_util_percent(65.0);
  saturated.mutable_disk_info(0)->set_avg_read_latency_ms(9.0);
  saturated.mutable_disk_info(0)->set_avg_write_latency_ms(9.0);
  saturated.mutable_diagnostic()->set_state(
      monitor::proto::OBSERVABILITY_DIAGNOSTIC);
  assert(manager.Submit(saturated) == monitor::DataReceiveResult::kAccepted);

  const auto score = WaitForScoreWhere(
      &manager, "disk-incident-host", [](const auto& current) {
        return current.info.has_diagnostic() &&
               current.info.diagnostic().state() ==
                   monitor::proto::OBSERVABILITY_DIAGNOSTIC;
      });
  bool saw_adaptive_disk_consensus = false;
  for (const auto& signal : score.health.top_signals) {
    if ((signal.metric == monitor::health::MetricId::kDiskUtil ||
         signal.metric == monitor::health::MetricId::kDiskLatency) &&
        signal.detector.threshold_score == 0.0 &&
        signal.detector.mad_score >= 0.5 &&
        signal.detector.ewma_score >= 0.5 &&
        signal.detector.anomaly_votes >= 2) {
      saw_adaptive_disk_consensus = true;
    }
  }
  assert(saw_adaptive_disk_consensus);

  std::vector<monitor::diagnostics::IncidentRecord> incidents;
  for (int attempt = 0; attempt < 300 && incidents.empty(); ++attempt) {
    incidents = manager.GetActiveIncidents("disk-incident-host");
    if (incidents.empty()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  }
  assert(incidents.size() == 1);
  const auto& incident = incidents.front();
  bool saw_disk_cause = false;
  for (const auto& cause : incidents.front().root_causes) {
    if (cause.type !=
        monitor::diagnostics::RootCauseType::kDiskIoSaturation) {
      continue;
    }
    assert(!cause.evidence_ids.empty());
    for (const auto& id : cause.evidence_ids) {
      bool matched_health_evidence = false;
      for (const auto& evidence : incident.evidence) {
        if (evidence.id == id) {
          assert(evidence.source == "HealthScoreEngine");
          matched_health_evidence = true;
        }
      }
      assert(matched_health_evidence);
    }
    saw_disk_cause = true;
  }
  for (const auto& evidence : incident.evidence) {
    if (evidence.source != "HealthScoreEngine" &&
        (evidence.type == monitor::diagnostics::EvidenceType::kIoWait ||
         evidence.type == monitor::diagnostics::EvidenceType::kDiskUtil ||
         evidence.type == monitor::diagnostics::EvidenceType::kDiskLatency)) {
      assert(evidence.severity < 0.5);
    }
  }
  assert(saw_disk_cause);
  manager.Stop();
}

monitor::proto::MonitorInfo MakeRelativeAnomalyInfo(const char* host,
                                                    bool stronger) {
  const float cpu = stronger ? 69.0F : 50.0F;
  const float load = stronger ? 0.9F : 0.6F;
  const float memory_used = stronger ? 79.0F : 60.0F;
  const double disk_util = stronger ? 69.0 : 50.0;
  const double disk_latency = stronger ? 9.9 : 5.0;
  const double packets = stronger ? 4950.0 : 3500.0;
  auto info = MakeInfo(host, 4, cpu, load);
  const float memory_avail = 16.0F * (100.0F - memory_used) / 100.0F;
  info.mutable_mem_info()->set_avail(memory_avail);
  info.mutable_mem_info()->set_free(memory_avail);
  info.mutable_mem_info()->set_used_percent(memory_used);
  info.mutable_disk_info(0)->set_util_percent(disk_util);
  info.mutable_disk_info(0)->set_avg_read_latency_ms(disk_latency);
  info.mutable_disk_info(0)->set_avg_write_latency_ms(disk_latency);
  info.mutable_net_info(0)->set_rcv_packets_rate(packets);
  info.mutable_net_info(0)->set_send_packets_rate(packets);
  return info;
}

void TestManagerFeedbackDrivesRelativeAnomalyState() {
  monitor::HostManager manager;
  assert(manager.Start());
  constexpr char kHost[] = "relative-feedback-host";
  constexpr char kKey[] = "relative-feedback-host_192.0.2.10";
  std::uint64_t sequence = 1;
  for (int index = 0; index < 31; ++index) {
    auto stable = MakeInfo(kHost, 4, 30.0F, 0.1F);
    stable.mutable_host_info()->set_hostname(kHost);
    stable.mutable_host_info()->set_ip_address("192.0.2.10");
    assert(monitor::CanonicalHostKey(stable) == kKey);
    SetSampleIdentity(&stable, sequence++);
    monitor::proto::MonitorFeedback feedback;
    assert(manager.SubmitWithFeedback(
               stable, std::chrono::system_clock::now() +
                           std::chrono::seconds(2),
               &feedback) == monitor::DataReceiveResult::kAccepted);
    assert(feedback.health_valid());
    assert(feedback.host_name() == kKey);
    assert(feedback.result_version() == stable.sample_sequence());
  }

  monitor::diagnostics::ObservabilityConfig config;
  monitor::diagnostics::AnomalyDetector detector(config);
  monitor::diagnostics::ObservabilityStateMachine state_machine(config);
  monitor::diagnostics::RemoteHealthFeedback remote(
      std::chrono::seconds(5));
  for (int index = 0; index < 2; ++index) {
    auto relative = MakeRelativeAnomalyInfo(kHost, index != 0);
    relative.mutable_host_info()->set_hostname(kHost);
    relative.mutable_host_info()->set_ip_address("192.0.2.10");
    SetSampleIdentity(&relative, sequence++);
    const auto local = detector.Evaluate(relative);
    assert(local.overall_score == 0.0);

    monitor::proto::MonitorFeedback feedback;
    assert(manager.SubmitWithFeedback(
               relative, std::chrono::system_clock::now() +
                             std::chrono::seconds(2),
               &feedback) == monitor::DataReceiveResult::kAccepted);
    assert(feedback.health_valid());
    assert(feedback.node_anomaly_score() >= config.suspect_enter_score);
    assert(remote.Accept(feedback, kKey, NowUnixMs()));
    const auto merged = remote.Merge(local, kKey, config);
    assert(merged.overall_score >= local.overall_score);
    state_machine.Update(merged);
  }
  assert(state_machine.state() ==
         monitor::diagnostics::ObservabilityState::kSuspect);
  manager.Stop();
}

void TestTrackedFeedbackIsPerWorkItemAndDeduplicated() {
  monitor::HostManager manager;
  assert(manager.Start());
  auto info = MakeInfo("dedup-host", 4, 30.0F, 0.1F);
  info.mutable_host_info()->set_hostname("dedup-host");
  info.mutable_host_info()->set_ip_address("192.0.2.20");
  constexpr char kKey[] = "dedup-host_192.0.2.20";
  SetSampleIdentity(&info, 7);

  std::array<monitor::proto::MonitorFeedback, 2> feedback;
  std::array<monitor::DataReceiveResult, 2> result;
  std::thread first([&] {
    result[0] = manager.SubmitWithFeedback(
        info, std::chrono::system_clock::now() + std::chrono::seconds(2),
        &feedback[0]);
  });
  std::thread second([&] {
    result[1] = manager.SubmitWithFeedback(
        info, std::chrono::system_clock::now() + std::chrono::seconds(2),
        &feedback[1]);
  });
  first.join();
  second.join();
  for (std::size_t index = 0; index < feedback.size(); ++index) {
    assert(result[index] == monitor::DataReceiveResult::kAccepted);
    assert(feedback[index].health_valid());
    assert(feedback[index].host_name() == kKey);
    assert(feedback[index].result_version() == 7);
    assert(feedback[index].result_timestamp_ms() ==
           info.sample_timestamp_ms());
  }
  assert(feedback[0].node_anomaly_score() ==
         feedback[1].node_anomaly_score());
  assert(manager.ProcessedCount() == 1);
  manager.Stop();
}

void TestSameHostnameDifferentIpIsIsolated() {
  constexpr char kHostname[] = "shared-hostname";
  constexpr char kFirstKey[] = "shared-hostname_192.0.2.31";
  constexpr char kSecondKey[] = "shared-hostname_192.0.2.32";
  const std::int64_t timestamp_ms = NowUnixMs();

  auto first = MakeInfo(kHostname, 4, 30.0F, 0.1F);
  first.mutable_host_info()->set_hostname(kHostname);
  first.mutable_host_info()->set_ip_address("192.0.2.31");
  first.set_sample_sequence(1);
  first.set_sample_timestamp_ms(timestamp_ms);
  auto second = first;
  second.mutable_host_info()->set_ip_address("192.0.2.32");
  assert(monitor::CanonicalHostKey(first) == kFirstKey);
  assert(monitor::CanonicalHostKey(second) == kSecondKey);

  auto only_hostname = first;
  only_hostname.mutable_host_info()->clear_ip_address();
  assert(monitor::CanonicalHostKey(only_hostname) == kHostname);
  auto only_ip = first;
  only_ip.mutable_host_info()->clear_hostname();
  assert(monitor::CanonicalHostKey(only_ip) == "192.0.2.31");
  auto name_fallback = first;
  name_fallback.clear_host_info();
  assert(monitor::CanonicalHostKey(name_fallback) == kHostname);

  monitor::HostManager manager;
  assert(manager.Start());
  monitor::proto::MonitorFeedback first_feedback;
  monitor::proto::MonitorFeedback second_feedback;
  const auto deadline =
      std::chrono::system_clock::now() + std::chrono::seconds(2);
  assert(manager.SubmitWithFeedback(first, deadline, &first_feedback) ==
         monitor::DataReceiveResult::kAccepted);
  assert(manager.SubmitWithFeedback(second, deadline, &second_feedback) ==
         monitor::DataReceiveResult::kAccepted);
  assert(first_feedback.host_name() == kFirstKey);
  assert(second_feedback.host_name() == kSecondKey);
  assert(manager.ProcessedCount() == 2);

  monitor::diagnostics::RemoteHealthFeedback first_remote(
      std::chrono::seconds(5));
  monitor::diagnostics::RemoteHealthFeedback second_remote(
      std::chrono::seconds(5));
  assert(first_remote.Accept(first_feedback, kFirstKey, timestamp_ms));
  assert(!first_remote.Accept(second_feedback, kFirstKey, timestamp_ms));
  assert(second_remote.Accept(second_feedback, kSecondKey, timestamp_ms));
  assert(!second_remote.Accept(first_feedback, kSecondKey, timestamp_ms));

  monitor::proto::MonitorFeedback first_retry;
  monitor::proto::MonitorFeedback second_retry;
  assert(manager.SubmitWithFeedback(first, deadline, &first_retry) ==
         monitor::DataReceiveResult::kAccepted);
  assert(manager.SubmitWithFeedback(second, deadline, &second_retry) ==
         monitor::DataReceiveResult::kAccepted);
  assert(first_retry.host_name() == kFirstKey);
  assert(second_retry.host_name() == kSecondKey);
  assert(manager.ProcessedCount() == 2);

  for (std::uint64_t sequence = 2; sequence <= 31; ++sequence) {
    auto stable = first;
    stable.set_sample_sequence(sequence);
    stable.set_sample_timestamp_ms(timestamp_ms + sequence);
    monitor::proto::MonitorFeedback feedback;
    assert(manager.SubmitWithFeedback(
               stable, std::chrono::system_clock::now() +
                           std::chrono::seconds(2),
               &feedback) == monitor::DataReceiveResult::kAccepted);
    assert(feedback.host_name() == kFirstKey);
  }
  const auto first_score = WaitForScore(&manager, kFirstKey);
  const auto second_score = WaitForScore(&manager, kSecondKey);
  assert(first_score.health.model_state ==
         monitor::health::ModelState::kReady);
  assert(second_score.health.model_state ==
         monitor::health::ModelState::kCold);

  auto incident_sample = first;
  incident_sample.set_sample_sequence(32);
  incident_sample.set_sample_timestamp_ms(timestamp_ms + 32);
  for (auto& cpu : *incident_sample.mutable_cpu_stat()) {
    cpu.set_cpu_percent(95.0F);
  }
  incident_sample.mutable_diagnostic()->set_state(
      monitor::proto::OBSERVABILITY_DIAGNOSTIC);
  monitor::proto::MonitorFeedback incident_feedback;
  assert(manager.SubmitWithFeedback(
             incident_sample, std::chrono::system_clock::now() +
                                  std::chrono::seconds(2),
             &incident_feedback) == monitor::DataReceiveResult::kAccepted);
  assert(incident_feedback.host_name() == kFirstKey);
  assert(manager.GetActiveIncidents(kFirstKey).size() == 1);
  assert(manager.GetActiveIncidents(kSecondKey).empty());
  assert(manager.ProcessedCount() == 33);
  manager.Stop();
}

void TestTimedOutFeedbackRetryUsesCachedWorkResult() {
  monitor::HostManager manager;
  assert(manager.Start());
  auto info = MakeInfo("retry-host", 4, 30.0F, 0.1F);
  SetSampleIdentity(&info, 11);
  monitor::proto::MonitorFeedback timed_out;
  assert(manager.SubmitWithFeedback(
             info, std::chrono::system_clock::time_point::min(), &timed_out) ==
         monitor::DataReceiveResult::kAccepted);
  const auto first_score = WaitForScore(&manager, "retry-host");
  assert(manager.ProcessedCount() == 1);

  monitor::proto::MonitorFeedback retry;
  assert(manager.SubmitWithFeedback(
             info, std::chrono::system_clock::now() +
                       std::chrono::seconds(2),
             &retry) == monitor::DataReceiveResult::kAccepted);
  assert(retry.health_valid());
  assert(retry.result_version() == info.sample_sequence());
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  const auto after_retry = WaitForScore(&manager, "retry-host");
  assert(after_retry.timestamp == first_score.timestamp);
  assert(manager.ProcessedCount() == 1);

  auto stale = info;
  stale.set_sample_sequence(12);
  stale.set_sample_timestamp_ms(info.sample_timestamp_ms() - 1);
  monitor::proto::MonitorFeedback stale_feedback;
  assert(manager.SubmitWithFeedback(
             stale, std::chrono::system_clock::now() +
                        std::chrono::seconds(2),
             &stale_feedback) == monitor::DataReceiveResult::kAccepted);
  assert(!stale_feedback.health_valid());
  assert(manager.ProcessedCount() == 1);

  auto restarted = info;
  restarted.set_sample_sequence(1);
  restarted.set_sample_timestamp_ms(info.sample_timestamp_ms() + 1);
  monitor::proto::MonitorFeedback restarted_feedback;
  assert(manager.SubmitWithFeedback(
             restarted, std::chrono::system_clock::now() +
                            std::chrono::seconds(2),
             &restarted_feedback) == monitor::DataReceiveResult::kAccepted);
  assert(restarted_feedback.health_valid());
  assert(restarted_feedback.result_version() == 1);
  assert(manager.ProcessedCount() == 2);
  manager.Stop();
}

void TestRelativeCpuTopSignalDrivesRca() {
  monitor::health::HealthScoreEngine engine;
  const auto start = monitor::health::HealthScoreEngine::Clock::time_point{};
  for (int index = 0; index < 31; ++index) {
    const auto stable = MakeInfo("relative-rca-host", 4, 30.0F, 0.1F);
    assert(engine
               .Evaluate(stable, start + std::chrono::seconds(index), 90.0)
               .valid);
  }
  auto relative = MakeInfo("relative-rca-host", 4, 60.0F, 0.1F);
  const auto health =
      engine.Evaluate(relative, start + std::chrono::seconds(32), 90.0);
  bool saw_consensus = false;
  for (const auto& signal : health.top_signals) {
    if ((signal.metric == monitor::health::MetricId::kCpuAverage ||
         signal.metric == monitor::health::MetricId::kCpuPeak) &&
        signal.detector.threshold_score == 0.0 &&
        signal.detector.mad_score >= 0.5 &&
        signal.detector.ewma_score >= 0.5 &&
        signal.detector.anomaly_votes >= 2) {
      saw_consensus = true;
    }
  }
  assert(saw_consensus);

  const auto timestamp = std::chrono::system_clock::now();
  monitor::diagnostics::EvidenceBuilder builder;
  const auto evidence = builder.Build(relative, timestamp, &health);
  monitor::diagnostics::RootCauseEngine root_cause_engine;
  const auto causes = root_cause_engine.Evaluate(evidence);
  bool saw_health_cpu = false;
  for (const auto& item : evidence) {
    if (item.type == monitor::diagnostics::EvidenceType::kCpuUsage &&
        item.source == "HealthScoreEngine" && item.severity >= 0.5) {
      saw_health_cpu = item.detail.find("threshold=0") != std::string::npos &&
                       item.detail.find("votes=2") != std::string::npos;
    }
  }
  assert(saw_health_cpu);
  bool saw_cpu_cause = false;
  for (const auto& cause : causes) {
    saw_cpu_cause =
        saw_cpu_cause ||
        cause.type == monitor::diagnostics::RootCauseType::kCpuSaturation;
  }
  assert(saw_cpu_cause);

  monitor::diagnostics::IncidentStore store;
  const auto incident =
      store.Observe("relative-rca-host", "SUSPECT", evidence, causes,
                    timestamp);
  assert(incident.has_value());
  assert(!incident->evidence.empty());
  assert(!incident->root_causes.empty());
}

void TestHealthStateExpiresWithoutNewTraffic() {
  monitor::HostManager manager(std::chrono::milliseconds(10),
                               std::chrono::milliseconds(30));
  assert(manager.Start());
  auto info = MakeInfo("no-traffic-stale-host", 2, 20.0F, 0.1F);
  assert(manager.Submit(info) == monitor::DataReceiveResult::kAccepted);
  WaitForScore(&manager, "no-traffic-stale-host");
  for (int attempt = 0;
       attempt < 300 && manager.HealthStateEvictedCount() == 0; ++attempt) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  assert(manager.HealthStateEvictedCount() == 1);
  manager.Stop();
}

}  // namespace

int main() {
  TestCpuOverviewAveragesAllCores();
  TestCpuOverviewFiltersNonFiniteSamples();
  TestCalcScoreUsesAverageCpu();
  TestCpuCountIsNotReducedForLoadScore();
  TestAllInvalidCpuScoreIsFinite();
  TestNegativeMemoryIsNotSchedulable();
  TestNetworkOverviewUsesAllInterfaces();
  TestManagerSizeConfigIsFailFast();
  TestManagerStartRejectsEveryInvalidSizeEnvironment();
  TestManagerStartRejectsInvalidHealthEnvironment();
  TestSoftIrqQueryUsesOnlyPerSecondFields();
  TestHostManagerSpikeRecovery();
  TestDiskIncidentIncludesHealthTopSignals();
  TestManagerFeedbackDrivesRelativeAnomalyState();
  TestTrackedFeedbackIsPerWorkItemAndDeduplicated();
  TestSameHostnameDifferentIpIsIsolated();
  TestTimedOutFeedbackRetryUsesCachedWorkResult();
  TestRelativeCpuTopSignalDrivesRca();
  TestHealthStateExpiresWithoutNewTraffic();
  return 0;
}
