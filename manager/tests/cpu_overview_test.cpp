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
#include <limits>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "host_manager.h"
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
  info.mutable_mem_info()->set_total(16.0F);
  info.mutable_mem_info()->set_free(16.0F);
  info.mutable_mem_info()->set_avail(16.0F);
  info.mutable_mem_info()->set_used_percent(0.0F);
  auto* net = info.add_net_info();
  net->set_name("eth0");
  auto* disk = info.add_disk_info();
  disk->set_name("sda");
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
  const auto score = WaitForScore(&manager, "average-host").score;
  manager.Stop();

  assert(std::isfinite(score));
  assert(std::abs(score - 82.5) < 0.01);
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
  TestSoftIrqQueryUsesOnlyPerSecondFields();
  return 0;
}
