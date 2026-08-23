#include <cassert>
#include <cmath>

#include "diagnostics/observability_config.h"
#include "monitor/cpu_softirq_monitor.h"
#include "monitor/cpu_stat_monitor.h"
#include "monitor/disk_monitor.h"
#include "monitor/net_monitor.h"

namespace {

void TestDiskFieldMapping() {
  monitor::DiskRawSample sample;
  assert(monitor::ParseDiskStatsLine(
      "8 0 sda 10 7 20 30 40 9 50 60 2 70 80 90 100 110", &sample));
  assert(sample.name == "sda");
  assert(sample.reads == 10);
  assert(sample.sectors_read == 20);
  assert(sample.read_time_ms == 30);
  assert(sample.writes == 40);
  assert(sample.sectors_written == 50);
  assert(sample.write_time_ms == 60);
  assert(sample.io_in_progress == 2);
  assert(sample.io_time_ms == 70);
  assert(sample.weighted_io_time_ms == 80);
  assert(!monitor::ParseDiskStatsLine("8 0 sda 1 2", &sample));
  assert(!monitor::ParseDiskStatsLine(
      "8 0 sda 1 -1 2 3 4 5 6 7 8 9 10", &sample));
  assert(!monitor::ParseDiskStatsLine(
      "8 0 sda extra 1 2 3 4 5 6 7 8 9 10 11", &sample));
}

void TestNetworkParser() {
  monitor::NetRawSample sample;
  assert(monitor::ParseNetDevLine(
      "eth0: 1024 2 3 4 0 0 0 0 2048 5 6 7 0 0 0 0", &sample));
  assert(sample.name == "eth0");
  assert(sample.rcv_bytes == 1024 && sample.rcv_packets == 2);
  assert(sample.snd_bytes == 2048 && sample.snd_packets == 5);
  assert(sample.err_in == 3 && sample.drop_out == 7);
  assert(!monitor::ParseNetDevLine("eth0: 1 2 3", &sample));
  assert(!monitor::ParseNetDevLine(
      "eth0: 1024 2 3 4 0 0 0 0 2048 5 6 7 0 0 0 0 trailing", &sample));
  assert(!monitor::ParseNetDevLine(
      "eth 0: 1024 2 3 4 0 0 0 0 2048 5 6 7 0 0 0 0", &sample));
  assert(!monitor::ParseNetDevLine(
      "eth0: 1024 -2 3 4 0 0 0 0 2048 5 6 7 0 0 0 0", &sample));
}

void TestCpuGuestReset() {
  monitor::cpu_stat_detail::CpuStatSnapshot previous;
  previous.user = 10;
  previous.guest = 20;
  previous.guest_nice = 30;
  auto current = previous;
  current.guest = 19;
  assert(monitor::cpu_stat_detail::ComputeCpuStatDelta(current, previous)
             .counter_reset);
  current = previous;
  current.guest_nice = 29;
  assert(monitor::cpu_stat_detail::ComputeCpuStatDelta(current, previous)
             .counter_reset);
}

void TestSoftIrqZeroDeltaIsReportable() {
  monitor::cpu_softirq_detail::Counters counters = {
      1ULL << 54, 2, 3, 4, 5, 6, 7, 8, 9, 10};
  monitor::cpu_softirq_detail::Rates rates{};
  assert(monitor::cpu_softirq_detail::ComputeRates(counters, counters, 1.0,
                                                    &rates));
  for (double rate : rates) assert(rate == 0.0);

  auto current = counters;
  current[0] += 1;
  assert(monitor::cpu_softirq_detail::ComputeRates(current, counters, 2.0,
                                                    &rates));
  assert(rates[0] == 0.5);

  current[0] = counters[0] - 1;
  assert(!monitor::cpu_softirq_detail::ComputeRates(current, counters, 1.0,
                                                     &rates));
}

void TestConfigRejectsFastSampling() {
  monitor::diagnostics::ObservabilityConfig config;
  config.normal_interval_ms = 999;
  assert(!config.IsValid());
  config.normal_interval_ms = 1000;
  assert(config.IsValid());
  config.profiling_duration_sec = config.profiling_max_duration_sec + 1;
  assert(!config.IsValid());
}

}  // namespace

int main() {
  TestDiskFieldMapping();
  TestNetworkParser();
  TestCpuGuestReset();
  TestSoftIrqZeroDeltaIsReportable();
  TestConfigRejectsFastSampling();
  return 0;
}
