#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>

#include "monitor/monitor_inter.h"
#include "monitor_info.pb.h"

namespace monitor {
class CpuStatMonitor : public MonitorInter {
  struct CpuStat {
    std::string cpu_name;
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

 public:
  CpuStatMonitor() {}
  void UpdateOnce(monitor::proto::MonitorInfo* monitor_info) override;
  void Stop() override {}

 private:
  std::unordered_map<std::string, struct CpuStat> cpu_stat_map_;
};

}  // namespace monitor
