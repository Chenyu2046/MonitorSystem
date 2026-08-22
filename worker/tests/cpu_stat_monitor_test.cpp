#include <cassert>
#include <cstdint>

#include "monitor/cpu_stat_monitor.h"

namespace {

using monitor::cpu_stat_detail::ComputeCpuStatDelta;
using monitor::cpu_stat_detail::CpuStatSnapshot;

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

void TestZeroDeltaIsNotReportable() {
  CpuStatSnapshot snapshot;
  const auto delta = ComputeCpuStatDelta(snapshot, snapshot);
  assert(!delta.counter_reset);
  assert(delta.total == 0);
  assert(!delta.IsReportable());
}

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
