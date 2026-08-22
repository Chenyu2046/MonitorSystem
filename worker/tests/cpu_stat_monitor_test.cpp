/**
 * @file cpu_stat_monitor_test.cpp
 * @brief 验证 CPU 累计计数器整数 delta、reset 和恢复语义。
 */

#include <cassert>
#include <cstdint>

#include "monitor/cpu_stat_monitor.h"

namespace {

using monitor::cpu_stat_detail::ComputeCpuStatDelta;
using monitor::cpu_stat_detail::CpuStatSnapshot;

/**
 * @brief 验证巨大累计值下的小增量仍能精确计算。
 * 回归风险：累计 uint64_t 若重新先转 float 再相减，会丢失短窗口增量。
 */
void TestLargeCountersKeepSmallDeltasExact() {
  CpuStatSnapshot previous;
  previous.user = 10'000'000'000ULL;
  previous.idle = 10'000'000'000ULL;

  CpuStatSnapshot current = previous;
  current.user += 10;
  current.idle += 90;

  const auto delta = ComputeCpuStatDelta(current, previous);
  assert(!delta.counter_reset);
  assert(delta.IsReportable());
  assert(delta.user == 10);
  assert(delta.idle == 90);
  assert(delta.total == 100);
  assert(delta.busy == 10);
  assert(static_cast<double>(delta.busy) / delta.total * 100.0 == 10.0);
}

/** @brief 验证零增量不生成可上报样本。 */
void TestZeroDeltaIsNotReportable() {
  CpuStatSnapshot snapshot;
  const auto delta = ComputeCpuStatDelta(snapshot, snapshot);
  assert(!delta.counter_reset);
  assert(delta.total == 0);
  assert(!delta.IsReportable());
}

/** @brief 验证 reset 不发生 uint64_t 下溢，并可从新基线恢复。 */
void TestCounterResetDoesNotUnderflow() {
  CpuStatSnapshot previous;
  previous.user = 1000;
  previous.idle = 1000;

  CpuStatSnapshot reset = previous;
  reset.user = 100;
  reset.idle = 100;
  const auto reset_delta = ComputeCpuStatDelta(reset, previous);
  assert(reset_delta.counter_reset);
  assert(reset_delta.total == 0);
  assert(!reset_delta.IsReportable());

  CpuStatSnapshot recovered = reset;
  recovered.user += 10;
  recovered.idle += 90;
  const auto recovered_delta = ComputeCpuStatDelta(recovered, reset);
  assert(!recovered_delta.counter_reset);
  assert(recovered_delta.IsReportable());
  assert(recovered_delta.total == 100);
}

}  // namespace

int main() {
  TestLargeCountersKeepSmallDeltasExact();
  TestZeroDeltaIsNotReportable();
  TestCounterResetDoesNotUnderflow();
  return 0;
}
