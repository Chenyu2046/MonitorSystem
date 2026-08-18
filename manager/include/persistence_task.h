#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>

#include "diagnostics/incident_store.h"
#include "monitor_info.pb.h"

namespace monitor {

struct HostScore {
  monitor::proto::MonitorInfo info;
  double score;
  std::chrono::system_clock::time_point timestamp;
};

struct PersistenceTask {
  std::string host_name;
  HostScore host_score;

  double net_in_rate = 0;
  double net_out_rate = 0;
  float cpu_percent_rate = 0;
  float usr_percent_rate = 0;
  float system_percent_rate = 0;
  float nice_percent_rate = 0;
  float idle_percent_rate = 0;
  float io_wait_percent_rate = 0;
  float irq_percent_rate = 0;
  float soft_irq_percent_rate = 0;
  float steal_percent_rate = 0;
  float guest_percent_rate = 0;
  float guest_nice_percent_rate = 0;
  float load_avg_1_rate = 0;
  float load_avg_3_rate = 0;
  float load_avg_15_rate = 0;
  float mem_used_percent_rate = 0;
  float mem_total_rate = 0;
  float mem_free_rate = 0;
  float mem_avail_rate = 0;
  float net_in_rate_rate = 0;
  float net_out_rate_rate = 0;
  float net_in_drop_rate_rate = 0;
  float net_out_drop_rate_rate = 0;

  std::optional<diagnostics::IncidentRecord> incident;
};

}  // namespace monitor
