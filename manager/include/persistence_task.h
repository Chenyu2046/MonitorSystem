#pragma once

/**
 * @file persistence_task.h
 * @brief Manager 异步普通指标/诊断持久化任务及其大小估算。
 *
 * PersistenceTask 携带 host 当前快照、变化率和可选 IncidentRecord，跨越
 * HostShardExecutor 与 PersistenceWorker 的线程边界。Estimate... 同时计
 * 算 protobuf 和诊断字符串占用，用于有界队列的字节背压，不改变数据库字段。
 */

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

#include "diagnostics/incident_store.h"
#include "monitor_info.pb.h"

namespace monitor {

/** @brief 主机当前 MonitorInfo、综合 score 和事件时间快照。 */
struct HostScore {
  monitor::proto::MonitorInfo info;
  double score;
  bool score_valid = false;
  std::chrono::system_clock::time_point timestamp;
};

/**
 * @brief 一个 host 的异步普通监控/诊断写入任务。
 *
 * rate 字段是当前样本相对 shard 缓存上一轮的变化率；incident 为空时
 * 只走普通历史写入，非空时还写诊断主表和 detail。
 */
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

/** @brief 估算任务跨队列占用，用于 item+byte 双重容量限制。 */
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
