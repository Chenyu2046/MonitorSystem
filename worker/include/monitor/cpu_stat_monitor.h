#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>

#include "monitor/monitor_inter.h"
#include "monitor_info.pb.h"

namespace monitor {

namespace cpu_stat_detail {

struct CpuStatSnapshot {
  std::uint64_t user = 0;
  std::uint64_t system = 0;
  std::uint64_t idle = 0;
  std::uint64_t nice = 0;
  std::uint64_t io_wait = 0;
  std::uint64_t irq = 0;
  std::uint64_t soft_irq = 0;
  std::uint64_t steal = 0;
  std::uint64_t guest = 0;
  std::uint64_t guest_nice = 0;
};

struct CpuStatDelta {
  bool counter_reset = false;
  std::uint64_t user = 0;
  std::uint64_t system = 0;
  std::uint64_t idle = 0;
  std::uint64_t nice = 0;
  std::uint64_t io_wait = 0;
  std::uint64_t irq = 0;
  std::uint64_t soft_irq = 0;
  std::uint64_t steal = 0;
  std::uint64_t total = 0;
  std::uint64_t busy = 0;

  bool IsReportable() const { return !counter_reset && total > 0; }
};

CpuStatDelta ComputeCpuStatDelta(const CpuStatSnapshot& current,
                                 const CpuStatSnapshot& previous);

}  // namespace cpu_stat_detail

class CpuStatMonitor : public MonitorInter {
 public:
  CpuStatMonitor() {}
  void UpdateOnce(monitor::proto::MonitorInfo* monitor_info) override;
  void Stop() override {}

 private:
  std::unordered_map<std::string, cpu_stat_detail::CpuStatSnapshot>
      cpu_stat_map_;
};

}  // namespace monitor
