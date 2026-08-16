#include <cassert>
#include <chrono>

#include "diagnostics/anomaly_detector.h"
#include "diagnostics/observability_config.h"
#include "diagnostics/observability_state.h"
#include "diagnostics/probe_controller.h"
#include "diagnostics/profile_session.h"
#include "diagnostics/symbolizer.h"

namespace {

using monitor::diagnostics::AnomalyDetector;
using monitor::diagnostics::AnomalyResult;
using monitor::diagnostics::DiagnosticSnapshot;
using monitor::diagnostics::ObservabilityConfig;
using monitor::diagnostics::ObservabilityState;
using monitor::diagnostics::ObservabilityStateMachine;
using monitor::diagnostics::ProbeController;
using monitor::diagnostics::ProbeKind;
using monitor::diagnostics::ProfileSession;
using monitor::diagnostics::ProfileType;
using monitor::diagnostics::Symbolizer;

monitor::proto::MonitorInfo MakeBaseInfo() {
  monitor::proto::MonitorInfo info;
  auto* cpu = info.add_cpu_stat();
  cpu->set_cpu_percent(20.0);
  cpu->set_io_wait_percent(2.0);
  cpu->set_soft_irq_percent(1.0);
  info.mutable_cpu_load()->set_load_avg_1(0.5);
  info.mutable_mem_info()->set_used_percent(40.0);
  auto* disk = info.add_disk_info();
  disk->set_util_percent(15.0);
  disk->set_avg_read_latency_ms(1.0);
  disk->set_avg_write_latency_ms(1.0);
  auto* net = info.add_net_info();
  net->set_rcv_packets_rate(10.0);
  net->set_send_packets_rate(10.0);
  auto* softirq = info.add_soft_irq();
  softirq->set_net_rx(10);
  softirq->set_net_tx(10);
  return info;
}

AnomalyResult HighAnomaly() {
  AnomalyResult result;
  result.overall_score = 1.0;
  result.should_diagnose = true;
  result.should_profile = true;
  return result;
}

AnomalyResult Recovery() { return {}; }

void TestAnomalyDetector() {
  AnomalyDetector detector;
  const auto normal = detector.Evaluate(MakeBaseInfo());
  assert(normal.overall_score < 0.6);
  assert(!normal.should_diagnose);

  auto cpu_info = MakeBaseInfo();
  cpu_info.mutable_cpu_stat(0)->set_cpu_percent(92.0);
  cpu_info.mutable_cpu_load()->set_load_avg_1(8.0);
  const auto cpu = detector.Evaluate(cpu_info);
  assert(cpu.should_diagnose);
  assert(cpu.should_profile);

  auto disk_info = MakeBaseInfo();
  disk_info.mutable_cpu_stat(0)->set_io_wait_percent(30.0);
  disk_info.mutable_disk_info(0)->set_util_percent(95.0);
  disk_info.mutable_disk_info(0)->set_avg_read_latency_ms(40.0);
  const auto disk = detector.Evaluate(disk_info);
  assert(disk.overall_score >= 0.8);
}

void TestStateMachineTransitions() {
  ObservabilityConfig config;
  assert(config.IsValid());
  ObservabilityStateMachine machine(config);
  const auto start = ObservabilityStateMachine::Clock::now();

  machine.Update(HighAnomaly(), start);
  assert(machine.state() == ObservabilityState::kNormal);
  machine.Update(HighAnomaly(), start + std::chrono::seconds(1));
  assert(machine.state() == ObservabilityState::kSuspect);
  machine.Update(HighAnomaly(), start + std::chrono::seconds(2));
  machine.Update(HighAnomaly(), start + std::chrono::seconds(3));
  assert(machine.state() == ObservabilityState::kDiagnostic);
  machine.Update(HighAnomaly(), start + std::chrono::seconds(4));
  assert(machine.state() == ObservabilityState::kProfiling);
  assert(machine.CurrentIntervalMs() == config.diagnostic_interval_ms);

  machine.Update(HighAnomaly(), start + std::chrono::seconds(20));
  assert(machine.state() == ObservabilityState::kCooldown);
  machine.Update(Recovery(), start + std::chrono::seconds(50));
  machine.Update(Recovery(), start + std::chrono::seconds(51));
  machine.Update(Recovery(), start + std::chrono::seconds(52));
  assert(machine.state() == ObservabilityState::kNormal);
  assert(machine.CurrentIntervalMs() == config.normal_interval_ms);
}

void TestRecoveryHysteresis() {
  ObservabilityStateMachine machine;
  const auto start = ObservabilityStateMachine::Clock::now();
  machine.Update(HighAnomaly(), start);
  machine.Update(HighAnomaly(), start + std::chrono::seconds(1));
  assert(machine.state() == ObservabilityState::kSuspect);
  machine.Update(Recovery(), start + std::chrono::seconds(2));
  machine.Update(Recovery(), start + std::chrono::seconds(3));
  assert(machine.state() == ObservabilityState::kSuspect);
  machine.Update(Recovery(), start + std::chrono::seconds(4));
  assert(machine.state() == ObservabilityState::kNormal);
}

void TestProbeController() {
  ProbeController controller;
  assert(controller.Apply(ObservabilityState::kNormal));
  assert(controller.DesiredProbes().empty());
  assert(controller.ApplyCount() == 1);
  assert(controller.Apply(ObservabilityState::kNormal));
  assert(controller.ApplyCount() == 1);
  assert(!controller.Apply(ObservabilityState::kSuspect));
  assert(controller.DesiredProbes().count(ProbeKind::kTcp) == 1);
  assert(controller.DesiredProbes().count(ProbeKind::kBlockIo) == 1);
  assert(!controller.Apply(ObservabilityState::kDiagnostic));
  assert(controller.DesiredProbes().count(ProbeKind::kScheduler) == 1);
  assert(controller.Apply(ObservabilityState::kCooldown));
  assert(controller.DesiredProbes().empty());
  DiagnosticSnapshot snapshot;
  assert(controller.CollectSnapshot(&snapshot));
  assert(!controller.Status(ProbeKind::kTcp).attached);

  const auto start = ProfileSession::Clock::now();
  assert(!controller.Apply(ObservabilityState::kProfiling, ProfileType::kOnCpu,
                           start));
  assert(controller.DesiredProbes().count(ProbeKind::kOnCpuProfile) == 1);
  assert(!controller.Apply(ObservabilityState::kProfiling, ProfileType::kOnCpu,
                           start + std::chrono::seconds(31)));
  assert(controller.DesiredProbes().count(ProbeKind::kOnCpuProfile) == 0);
}

void TestProfileSession() {
  bool detached = false;
  ProfileSession session(1, ProfileType::kOnCpu, std::chrono::seconds(2),
                         std::nullopt, [&] { detached = true; });
  const auto start = ProfileSession::Clock::now();
  session.Start(start);
  assert(session.active());
  assert(!session.Expired(start + std::chrono::seconds(1)));
  assert(session.Expired(start + std::chrono::seconds(2)));
  session.Close();
  assert(!session.active());
  assert(detached);
}

void TestSymbolizerFallback() {
  Symbolizer symbolizer;
  assert(!symbolizer.LoadKernelSymbols("missing-kallsyms"));
  assert(symbolizer.SymbolizeKernel(0x1234) == "0x1234");
  assert(!symbolizer.SymbolizeUser(1, 0x1234).empty());
}

}  // namespace

int main() {
  TestAnomalyDetector();
  TestStateMachineTransitions();
  TestRecoveryHysteresis();
  TestProbeController();
  TestProfileSession();
  TestSymbolizerFallback();
  return 0;
}
