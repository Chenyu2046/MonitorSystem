#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>

#include "diagnostics/anomaly_detector.h"
#include "diagnostics/observability_config.h"
#include "diagnostics/observability_state.h"
#include "diagnostics/probe_controller.h"
#include "diagnostics/profile_session.h"
#include "diagnostics/symbolizer.h"
#include "rpc/monitor_send_queue.h"

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
  machine.Update(HighAnomaly(), start + std::chrono::seconds(21));
  assert(machine.state() == ObservabilityState::kCooldown);
  machine.Update(HighAnomaly(), start + std::chrono::seconds(51));
  assert(machine.state() == ObservabilityState::kCooldown);
  machine.Update(HighAnomaly(), start + std::chrono::seconds(52));
  assert(machine.state() == ObservabilityState::kDiagnostic);

  machine.Update(HighAnomaly(), start + std::chrono::seconds(53));
  assert(machine.state() == ObservabilityState::kProfiling);
  machine.Update(Recovery(), start + std::chrono::seconds(54));
  assert(machine.state() == ObservabilityState::kCooldown);
  machine.Update(Recovery(), start + std::chrono::seconds(55));
  machine.Update(Recovery(), start + std::chrono::seconds(83));
  machine.Update(Recovery(), start + std::chrono::seconds(84));
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
  assert(!controller.Apply(ObservabilityState::kSuspect));
  assert(controller.ApplyCount() == 2);
  assert(!controller.Apply(ObservabilityState::kDiagnostic));
  assert(controller.DesiredProbes().count(ProbeKind::kScheduler) == 1);
  assert(!controller.Apply(ObservabilityState::kCooldown));
  assert(controller.DesiredProbes().count(ProbeKind::kTcp) == 1);
  assert(controller.DesiredProbes().count(ProbeKind::kBlockIo) == 1);
  assert(controller.DesiredProbes().count(ProbeKind::kScheduler) == 1);
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

  const auto symbols_path =
      std::filesystem::temp_directory_path() / "kernscope-kallsyms-test.txt";
  {
    std::ofstream output(symbols_path);
    output << "1000 T test_symbol\n";
  }
  Symbolizer loaded;
  assert(loaded.LoadKernelSymbols(symbols_path.string()));
  assert(loaded.SymbolizeKernel(0x1010) == "test_symbol+0x10");
  std::filesystem::remove(symbols_path);
}

void TestProfileStackFrames() {
  monitor::diagnostics::OnCpuProfileSample sample;
  sample.user_stack_id = -1;
  sample.kernel_stack_id = -1;
  assert(sample.user_stack.empty());
  assert(sample.kernel_stack.empty());

  monitor::proto::ProfileEntry entry;
  entry.set_user_stack_id(sample.user_stack_id);
  entry.set_kernel_stack_id(sample.kernel_stack_id);
  auto* user_frame = entry.add_user_stack();
  user_frame->set_address(0x1234);
  user_frame->set_symbol("module+0x34");
  auto* kernel_frame = entry.add_kernel_stack();
  kernel_frame->set_address(0x5678);
  kernel_frame->set_symbol("schedule+0x78");
  assert(entry.user_stack_size() == 1);
  assert(entry.kernel_stack_size() == 1);
}

void TestProbeRuntimeStatusProto() {
  monitor::proto::DiagnosticSnapshot diagnostic;
  auto* status = diagnostic.add_probe_status();
  status->set_probe("TCP");
  status->set_requested(true);
  status->set_available(false);
  status->set_attached(false);
  status->set_last_error(-95);
  assert(diagnostic.probe_status_size() == 1);
  assert(diagnostic.probe_status(0).requested());
  assert(!diagnostic.probe_status(0).available());
  assert(diagnostic.probe_status(0).last_error() == -95);
}

void TestMonitorSendQueuePriority() {
  monitor::MonitorSendQueue queue(2, 1024);
  queue.Open();

  monitor::proto::MonitorInfo base_one;
  base_one.set_name("base-one");
  monitor::proto::MonitorInfo base_two;
  base_two.set_name("base-two");
  monitor::proto::MonitorInfo diagnostic;
  diagnostic.set_name("diagnostic");
  diagnostic.mutable_diagnostic()->set_state(
      monitor::proto::OBSERVABILITY_DIAGNOSTIC);

  assert(queue.Push(std::move(base_one)));
  assert(queue.Push(std::move(base_two)));
  assert(queue.Push(std::move(diagnostic)));
  assert(queue.dropped_count() == 1);

  monitor::proto::MonitorInfo output;
  assert(queue.Pop(&output));
  assert(output.name() == "base-two");
  queue.Close();
  assert(queue.Pop(&output));
  assert(output.name() == "diagnostic");
  assert(!queue.Pop(&output));

  monitor::MonitorSendQueue byte_limited(2, 1);
  byte_limited.Open();
  monitor::proto::MonitorInfo oversized;
  oversized.set_name("x");
  assert(!byte_limited.Push(std::move(oversized)));
  assert(byte_limited.dropped_count() == 1);
  byte_limited.Close();

  monitor::MonitorSendQueue drain(2, 1024);
  drain.Open();
  monitor::proto::MonitorInfo pending;
  pending.set_name("pending");
  assert(drain.Push(std::move(pending)));
  drain.Close();
  assert(drain.Pop(&output));
  assert(output.name() == "pending");
  assert(!drain.Pop(&output));
}

}  // namespace

int main() {
  TestAnomalyDetector();
  TestStateMachineTransitions();
  TestRecoveryHysteresis();
  TestProbeController();
  TestProfileSession();
  TestSymbolizerFallback();
  TestProfileStackFrames();
  TestProbeRuntimeStatusProto();
  TestMonitorSendQueuePriority();
  return 0;
}
