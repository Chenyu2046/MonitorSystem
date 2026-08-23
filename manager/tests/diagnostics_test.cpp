/**
 * @file diagnostics_test.cpp
 * @brief 验证 Evidence/RootCause/Incident、Probe 降级和持久化状态语义。
 *
 * 测试输入来自构造的 MonitorInfo/DiagnosticSnapshot，防止把平均 CPU、
 * profiling 样本、lock wait 和 MySQL 降级状态解释错误。
 */

#include <cassert>
#include <chrono>
#include <string>

#include "diagnostics/evidence_builder.h"
#include "diagnostics/incident_store.h"
#include "diagnostics/root_cause_engine.h"
#include "host_manager.h"
#include "mysql_timeout_config.h"

namespace {

/** @brief 构造带 CPU、load 和 OnCPU stack 的诊断输入。 */
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

/** @brief 验证 CPU evidence 取 peak core 而不是平均值。 */
void TestCpuEvidenceUsesPeakCore() {
  monitor::proto::MonitorInfo info;
  info.add_cpu_stat()->set_cpu_percent(20.0);
  info.add_cpu_stat()->set_cpu_percent(95.0);

  monitor::diagnostics::EvidenceBuilder builder;
  const auto evidence = builder.Build(info, std::chrono::system_clock::now());
  for (const auto& item : evidence) {
    if (item.type == monitor::diagnostics::EvidenceType::kCpuUsage) {
      assert(item.value == 95.0);
      return;
    }
  }
  assert(false);
}

/** @brief 验证 CPU 根因需要多条组合证据。 */
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

/** @brief 验证磁盘组合根因和 incident active/close/history。 */
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

/** @brief 验证网络栈压力需要包速率、SoftIRQ、重传等多证据。 */
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

/** @brief 验证无 stack frame 的 profiling 样本不虚构 stack evidence。 */
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

/** @brief 验证锁竞争规则必须命中 lock-wait stack。 */
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

/** @brief 验证普通磁盘等待不会误判为锁竞争。 */
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

/** @brief 验证 requested 但 unavailable/未 attach 的 Probe 产生降级证据。 */
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

/** @brief 已 attach 但本轮 map 读取失败时不能伪装成无证据。 */
void TestProbeSnapshotReadFailureIsDegradedEvidence() {
  monitor::proto::MonitorInfo info;
  auto* status = info.mutable_diagnostic()->add_probe_status();
  status->set_probe("TCP");
  status->set_requested(true);
  status->set_available(true);
  status->set_attached(true);
  status->set_last_error(0);
  status->set_snapshot_ok(false);
  status->set_snapshot_error(-5);

  monitor::diagnostics::EvidenceBuilder builder;
  const auto evidence =
      builder.Build(info, std::chrono::system_clock::now());
  assert(evidence.size() == 1);
  assert(evidence.front().type ==
         monitor::diagnostics::EvidenceType::kDiagnosticCapabilityDegraded);
}

/** @brief 验证单个 incident 持久化失败会保持 degraded 状态。 */
void TestPersistenceFailureRemainsDegradedAfterOtherSuccess() {
  monitor::DiagnosticPersistenceState state;
  state.SetInitialized(true);
  state.RecordSave(100, false);
  state.RecordSave(200, true);
  assert(state.IsDegraded());
  state.RecordSave(100, true);
  assert(!state.IsDegraded());
}

/** @brief 验证未初始化 MySQL 时内存路径不累积伪 pending incident。 */
void TestMemoryOnlyPersistenceDoesNotAccumulatePendingIncidents() {
  monitor::DiagnosticPersistenceState state;
  state.SetInitialized(false);
  state.RecordSave(1, false);
  state.RecordSave(2, false);
  state.RecordSave(3, false);
  state.SetInitialized(true);
  assert(!state.IsDegraded());
}

/** @brief 验证 MySQL timeout 配置解析边界。 */
void TestMysqlTimeoutParsing() {
  assert(monitor::ParseMysqlTimeoutSeconds("5").value() == 5);
  assert(monitor::ParseMysqlTimeoutSeconds("0") == std::nullopt);
  assert(monitor::ParseMysqlTimeoutSeconds("-1") == std::nullopt);
  assert(monitor::ParseMysqlTimeoutSeconds("not-a-number") == std::nullopt);
  assert(monitor::ParseMysqlTimeoutSeconds("4294967296") == std::nullopt);
}

}  // namespace

int main() {
  TestCpuEvidenceUsesPeakCore();
  TestCpuRuleRequiresMultipleSignals();
  TestDiskRuleAndIncidentStore();
  TestNetworkEvidenceRequiresAllSignals();
  TestEmptyProfilesDoNotBecomeStackEvidence();
  TestLockContentionRequiresLockWaitStack();
  TestDiskWaitDoesNotBecomeLockContention();
  TestProbeCapabilityDegradedEvidence();
  TestProbeSnapshotReadFailureIsDegradedEvidence();
  TestPersistenceFailureRemainsDegradedAfterOtherSuccess();
  TestMemoryOnlyPersistenceDoesNotAccumulatePendingIncidents();
  TestMysqlTimeoutParsing();
  return 0;
}
