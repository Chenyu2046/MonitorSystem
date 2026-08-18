#include <cassert>
#include <chrono>
#include <string>

#include "diagnostics/evidence_builder.h"
#include "diagnostics/incident_store.h"
#include "diagnostics/root_cause_engine.h"
#include "host_manager.h"
#include "mysql_timeout_config.h"

namespace {

monitor::proto::MonitorInfo MakeCpuProfileInfo() {
  monitor::proto::MonitorInfo info;
  auto* cpu = info.add_cpu_stat();
  cpu->set_cpu_percent(92.0);
  cpu->set_io_wait_percent(2.0);
  info.mutable_cpu_load()->set_load_avg_1(8.0);
  auto* diagnostic = info.mutable_diagnostic();
  diagnostic->set_state(monitor::proto::OBSERVABILITY_PROFILING);
  auto* profile = diagnostic->add_oncpu_profiles();
  profile->set_pid(100);
  profile->set_tid(101);
  profile->set_samples(200);
  auto* user_frame = profile->add_user_stack();
  user_frame->set_address(0x1234);
  user_frame->set_symbol("busy_loop+0x4");
  auto* kernel_frame = profile->add_kernel_stack();
  kernel_frame->set_address(0x5678);
  kernel_frame->set_symbol("finish_task_switch+0x8");
  return info;
}

void TestCpuRuleRequiresMultipleSignals() {
  monitor::diagnostics::EvidenceBuilder builder;
  monitor::diagnostics::RootCauseEngine engine;
  const auto timestamp = std::chrono::system_clock::now();

  const auto evidence = builder.Build(MakeCpuProfileInfo(), timestamp);
  const auto causes = engine.Evaluate(evidence);
  assert(!causes.empty());
  assert(causes.front().type ==
         monitor::diagnostics::RootCauseType::kCpuSaturation);
  assert(causes.front().confidence >= 0.75);
  bool saw_stack_detail = false;
  for (const auto& item : evidence) {
    if (item.type == monitor::diagnostics::EvidenceType::kOnCpuStack) {
      saw_stack_detail = item.detail.find("busy_loop+0x4") != std::string::npos &&
                         item.detail.find("finish_task_switch+0x8") !=
                             std::string::npos;
    }
  }
  assert(saw_stack_detail);

  monitor::proto::MonitorInfo cpu_only;
  auto* cpu = cpu_only.add_cpu_stat();
  cpu->set_cpu_percent(95.0);
  const auto insufficient = builder.Build(cpu_only, timestamp);
  assert(engine.Evaluate(insufficient).empty());
}

void TestDiskRuleAndIncidentStore() {
  monitor::proto::MonitorInfo info;
  auto* cpu = info.add_cpu_stat();
  cpu->set_io_wait_percent(30.0);
  auto* disk = info.add_disk_info();
  disk->set_util_percent(98.0);
  disk->set_avg_read_latency_ms(50.0);
  auto* diagnostic = info.mutable_diagnostic();
  diagnostic->set_state(monitor::proto::OBSERVABILITY_DIAGNOSTIC);
  auto* profile = diagnostic->add_offcpu_profiles();
  profile->set_pid(200);
  profile->set_total_offcpu_ns(1000000000);
  auto* block_signal = diagnostic->add_signals();
  block_signal->set_metric("block_io_avg_latency_ms");
  block_signal->set_value(50.0);
  block_signal->set_unit("ms");
  block_signal->set_target("host");

  const auto timestamp = std::chrono::system_clock::now();
  monitor::diagnostics::EvidenceBuilder builder;
  monitor::diagnostics::RootCauseEngine engine;
  const auto evidence = builder.Build(info, timestamp);
  const auto causes = engine.Evaluate(evidence);
  assert(!causes.empty());
  assert(causes.front().type ==
         monitor::diagnostics::RootCauseType::kDiskIoSaturation);
  bool saw_bpf_block_latency = false;
  for (const auto& item : evidence) {
    saw_bpf_block_latency =
        saw_bpf_block_latency ||
        item.type == monitor::diagnostics::EvidenceType::kBpfBlockLatency;
  }
  assert(saw_bpf_block_latency);

  monitor::diagnostics::IncidentStore store;
  store.Observe("host-a", "DIAGNOSTIC", evidence, causes, timestamp);
  const auto active = store.Active("host-a");
  assert(active.size() == 1);
  assert(active.front().active);
  assert(store.Get(active.front().id).has_value());
  store.Observe("host-a", "COOLDOWN", evidence, {},
                timestamp + std::chrono::seconds(1));
  const auto history = store.List("host-a");
  assert(history.size() == 1);
  assert(!history.front().active);
}

void TestNetworkEvidenceRequiresAllSignals() {
  monitor::proto::MonitorInfo info;
  auto* net = info.add_net_info();
  net->set_rcv_packets_rate(120000.0);
  auto* softirq = info.add_soft_irq();
  softirq->set_net_rx(120000);
  auto* diagnostic = info.mutable_diagnostic();
  auto* retrans = diagnostic->add_signals();
  retrans->set_metric("tcp_retransmissions");
  retrans->set_value(3.0);
  retrans->set_unit("count");
  retrans->set_target("pid:7/tgid:7");

  monitor::diagnostics::EvidenceBuilder builder;
  monitor::diagnostics::RootCauseEngine engine;
  const auto timestamp = std::chrono::system_clock::now();
  const auto evidence = builder.Build(info, timestamp);
  const auto causes = engine.Evaluate(evidence);
  assert(!causes.empty());
  assert(causes.front().type ==
         monitor::diagnostics::RootCauseType::kNetworkStackPressure);
  bool saw_target = false;
  for (const auto& item : evidence) {
    if (item.type == monitor::diagnostics::EvidenceType::kTcpRetrans) {
      saw_target = item.target == "pid:7/tgid:7";
    }
  }
  assert(saw_target);

  monitor::proto::MonitorInfo single_signal;
  auto* single_diagnostic = single_signal.mutable_diagnostic();
  auto* single_retrans = single_diagnostic->add_signals();
  single_retrans->set_metric("tcp_retransmissions");
  single_retrans->set_value(3.0);
  const auto insufficient = builder.Build(single_signal, timestamp);
  assert(engine.Evaluate(insufficient).empty());
}

bool HasEvidence(const std::vector<monitor::diagnostics::Evidence>& evidence,
                 monitor::diagnostics::EvidenceType type) {
  for (const auto& item : evidence) {
    if (item.type == type) return true;
  }
  return false;
}

bool HasCause(const std::vector<monitor::diagnostics::RootCause>& causes,
             monitor::diagnostics::RootCauseType type) {
  for (const auto& cause : causes) {
    if (cause.type == type) return true;
  }
  return false;
}

void TestEmptyProfilesDoNotBecomeStackEvidence() {
  monitor::proto::MonitorInfo info;
  auto* cpu = info.add_cpu_stat();
  cpu->set_cpu_percent(20.0);
  auto* diagnostic = info.mutable_diagnostic();
  auto* on_cpu = diagnostic->add_oncpu_profiles();
  on_cpu->set_samples(10);
  auto* off_cpu = diagnostic->add_offcpu_profiles();
  off_cpu->set_total_offcpu_ns(2000000000);

  monitor::diagnostics::EvidenceBuilder builder;
  monitor::diagnostics::RootCauseEngine engine;
  const auto evidence =
      builder.Build(info, std::chrono::system_clock::now());
  assert(!HasEvidence(evidence,
                      monitor::diagnostics::EvidenceType::kOnCpuStack));
  assert(!HasEvidence(evidence,
                      monitor::diagnostics::EvidenceType::kOffCpuStack));
  assert(!HasEvidence(evidence,
                      monitor::diagnostics::EvidenceType::kLockWaitStack));
  assert(!HasCause(engine.Evaluate(evidence),
                   monitor::diagnostics::RootCauseType::kLockContention));
}

void TestLockContentionRequiresLockWaitStack() {
  monitor::proto::MonitorInfo info;
  auto* cpu = info.add_cpu_stat();
  cpu->set_cpu_percent(20.0);
  auto* profile = info.mutable_diagnostic()->add_offcpu_profiles();
  profile->set_pid(300);
  profile->set_samples(10);
  profile->set_total_offcpu_ns(2000000000);
  profile->add_kernel_stack()->set_symbol("futex_wait");
  profile->add_kernel_stack()->set_symbol("pthread_mutex_lock");

  monitor::diagnostics::EvidenceBuilder builder;
  monitor::diagnostics::RootCauseEngine engine;
  const auto evidence =
      builder.Build(info, std::chrono::system_clock::now());
  assert(HasEvidence(evidence,
                     monitor::diagnostics::EvidenceType::kOffCpuStack));
  assert(HasEvidence(evidence,
                     monitor::diagnostics::EvidenceType::kLockWaitStack));
  assert(HasCause(engine.Evaluate(evidence),
                  monitor::diagnostics::RootCauseType::kLockContention));
}

void TestDiskWaitDoesNotBecomeLockContention() {
  monitor::proto::MonitorInfo info;
  auto* cpu = info.add_cpu_stat();
  cpu->set_cpu_percent(20.0);
  cpu->set_io_wait_percent(30.0);
  auto* disk = info.add_disk_info();
  disk->set_util_percent(98.0);
  disk->set_avg_read_latency_ms(50.0);
  auto* profile = info.mutable_diagnostic()->add_offcpu_profiles();
  profile->set_samples(10);
  profile->set_total_offcpu_ns(2000000000);
  profile->add_kernel_stack()->set_symbol("io_schedule");

  monitor::diagnostics::EvidenceBuilder builder;
  monitor::diagnostics::RootCauseEngine engine;
  const auto causes =
      engine.Evaluate(builder.Build(info, std::chrono::system_clock::now()));
  assert(HasCause(causes,
                  monitor::diagnostics::RootCauseType::kDiskIoSaturation));
  assert(!HasCause(causes,
                   monitor::diagnostics::RootCauseType::kLockContention));
}

void TestProbeCapabilityDegradedEvidence() {
  monitor::proto::MonitorInfo info;
  auto* status = info.mutable_diagnostic()->add_probe_status();
  status->set_probe("BLOCK_IO");
  status->set_requested(true);
  status->set_available(false);
  status->set_attached(false);
  status->set_last_error(-95);

  monitor::diagnostics::EvidenceBuilder builder;
  const auto evidence =
      builder.Build(info, std::chrono::system_clock::now());
  assert(evidence.size() == 1);
  assert(evidence.front().type ==
         monitor::diagnostics::EvidenceType::kDiagnosticCapabilityDegraded);
  monitor::diagnostics::RootCauseEngine engine;
  assert(engine.Evaluate(evidence).empty());
}

void TestPersistenceFailureRemainsDegradedAfterOtherSuccess() {
  monitor::DiagnosticPersistenceState state;
  state.SetInitialized(true);
  state.RecordSave(100, false);
  state.RecordSave(200, true);
  assert(state.IsDegraded());
  state.RecordSave(100, true);
  assert(!state.IsDegraded());
}

void TestMemoryOnlyPersistenceDoesNotAccumulatePendingIncidents() {
  monitor::DiagnosticPersistenceState state;
  state.SetInitialized(false);
  state.RecordSave(1, false);
  state.RecordSave(2, false);
  state.RecordSave(3, false);
  state.SetInitialized(true);
  assert(!state.IsDegraded());
}

void TestMysqlTimeoutParsing() {
  assert(monitor::ParseMysqlTimeoutSeconds("5").value() == 5);
  assert(monitor::ParseMysqlTimeoutSeconds("0") == std::nullopt);
  assert(monitor::ParseMysqlTimeoutSeconds("-1") == std::nullopt);
  assert(monitor::ParseMysqlTimeoutSeconds("not-a-number") == std::nullopt);
  assert(monitor::ParseMysqlTimeoutSeconds("4294967296") == std::nullopt);
}

}  // namespace

int main() {
  TestCpuRuleRequiresMultipleSignals();
  TestDiskRuleAndIncidentStore();
  TestNetworkEvidenceRequiresAllSignals();
  TestEmptyProfilesDoNotBecomeStackEvidence();
  TestLockContentionRequiresLockWaitStack();
  TestDiskWaitDoesNotBecomeLockContention();
  TestProbeCapabilityDegradedEvidence();
  TestPersistenceFailureRemainsDegradedAfterOtherSuccess();
  TestMemoryOnlyPersistenceDoesNotAccumulatePendingIncidents();
  TestMysqlTimeoutParsing();
  return 0;
}
