#include <cassert>
#include <chrono>
#include <string>

#include "diagnostics/evidence_builder.h"
#include "diagnostics/incident_store.h"
#include "diagnostics/root_cause_engine.h"

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

  const auto timestamp = std::chrono::system_clock::now();
  monitor::diagnostics::EvidenceBuilder builder;
  monitor::diagnostics::RootCauseEngine engine;
  const auto evidence = builder.Build(info, timestamp);
  const auto causes = engine.Evaluate(evidence);
  assert(!causes.empty());
  assert(causes.front().type ==
         monitor::diagnostics::RootCauseType::kDiskIoSaturation);

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

}  // namespace

int main() {
  TestCpuRuleRequiresMultipleSignals();
  TestDiskRuleAndIncidentStore();
  return 0;
}
