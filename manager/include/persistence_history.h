#pragma once

#include <cstdint>
#include <map>
#include <string>

namespace monitor {

struct NetDetailSample {
  float rcv_bytes_rate = 0;
  float rcv_packets_rate = 0;
  float snd_bytes_rate = 0;
  float snd_packets_rate = 0;
  std::uint64_t err_in = 0;
  std::uint64_t err_out = 0;
  std::uint64_t drop_in = 0;
  std::uint64_t drop_out = 0;
};

struct SoftIrqSample {
  float hi = 0, timer = 0, net_tx = 0, net_rx = 0, block = 0;
  float irq_poll = 0, tasklet = 0, sched = 0, hrtimer = 0, rcu = 0;
};

struct MemDetailSample {
  float total = 0, free = 0, avail = 0, buffers = 0, cached = 0;
  float swap_cached = 0, active = 0, inactive = 0;
  float active_anon = 0, inactive_anon = 0, active_file = 0, inactive_file = 0;
  float dirty = 0, writeback = 0, anon_pages = 0, mapped = 0;
  float kreclaimable = 0, sreclaimable = 0, sunreclaim = 0;
};

struct DiskDetailSample {
  float read_bytes_per_sec = 0;
  float write_bytes_per_sec = 0;
  float read_iops = 0;
  float write_iops = 0;
  float avg_read_latency_ms = 0;
  float avg_write_latency_ms = 0;
  float util_percent = 0;
};

struct PersistenceHistory {
  std::map<std::string, std::map<std::string, NetDetailSample>> net_samples;
  std::map<std::string, std::map<std::string, SoftIrqSample>> softirq_samples;
  std::map<std::string, MemDetailSample> mem_samples;
  std::map<std::string, std::map<std::string, DiskDetailSample>> disk_samples;
  std::map<std::string, float> disk_util;
};

}  // namespace monitor
