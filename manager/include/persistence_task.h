#pragma once

#include <chrono>
#include <cstddef>
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

inline std::size_t EstimatePersistenceTaskBytes(const PersistenceTask& task) {
  std::size_t bytes = sizeof(PersistenceTask) + task.host_name.size() +
                      task.host_score.info.ByteSizeLong();
  if (!task.incident) {
    return bytes;
  }

  const auto& incident = *task.incident;
  bytes += incident.server_name.size() + incident.severity.size() +
           incident.state.size();
  for (const auto& cause : incident.root_causes) {
    bytes += sizeof(cause) + cause.summary.size();
    for (const auto& evidence_id : cause.evidence_ids) {
      bytes += evidence_id.size();
    }
  }
  for (const auto& evidence : incident.evidence) {
    bytes += sizeof(evidence) + evidence.id.size() + evidence.source.size() +
             evidence.target.size() + evidence.unit.size() +
             evidence.detail.size();
  }
  return bytes;
}

}  // namespace monitor
