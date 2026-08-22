#include <cassert>
#include <chrono>
#include <cmath>
#include <limits>
#include <string>
#include <thread>

#include "host_manager.h"

namespace {

monitor::proto::MonitorInfo MakeInfo(const char* name, int cpu_count,
                                     float cpu_percent, float load_avg_1 = 0) {
  monitor::proto::MonitorInfo info;
  info.set_name(name);
  info.mutable_cpu_load()->set_load_avg_1(load_avg_1);
  for (int index = 0; index < cpu_count; ++index) {
    auto* cpu = info.add_cpu_stat();
    cpu->set_cpu_name("cpu" + std::to_string(index));
    cpu->set_cpu_percent(cpu_percent);
  }
  return info;
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

void TestCalcScoreUsesAverageCpu() {
  monitor::HostManager manager;
  manager.Start();

  auto info = MakeInfo("average-host", 2, 10.0F);
  info.mutable_cpu_stat(1)->set_cpu_percent(90.0F);
  assert(manager.Submit(info) == monitor::DataReceiveResult::kAccepted);
  const auto score = WaitForScore(&manager, "average-host").score;
  manager.Stop();

  assert(std::isfinite(score));
  assert(std::abs(score - 82.5) < 0.01);
}

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
  manager.Stop();

  assert(std::isfinite(score));
}

}  // namespace

int main() {
  TestCpuOverviewAveragesAllCores();
  TestCpuOverviewFiltersNonFiniteSamples();
  TestCalcScoreUsesAverageCpu();
  TestCpuCountIsNotReducedForLoadScore();
  TestAllInvalidCpuScoreIsFinite();
  return 0;
}
