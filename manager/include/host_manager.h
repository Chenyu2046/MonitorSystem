#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "data_receive_result.h"
#include "diagnostics/diagnostic_persistence.h"
#include "diagnostics/incident_store.h"
#include "host_shard_executor.h"
#include "persistence_history.h"
#include "persistence_task.h"
#include "persistence_worker.h"

#include "monitor_info.pb.h"

namespace monitor {

struct PerfSample {
  float cpu_percent = 0, usr_percent = 0, system_percent = 0;
  float nice_percent = 0, idle_percent = 0, io_wait_percent = 0;
  float irq_percent = 0, soft_irq_percent = 0;
  float steal_percent = 0, guest_percent = 0, guest_nice_percent = 0;
  float load_avg_1 = 0, load_avg_3 = 0, load_avg_15 = 0;
  float mem_used_percent = 0, mem_total = 0, mem_free = 0, mem_avail = 0;
  float net_in_rate = 0, net_out_rate = 0;
  float score = 0;
};

class DiagnosticPersistenceState {
 public:
  void SetInitialized(bool initialized) {
    std::lock_guard<std::mutex> lock(mutex_);
    initialized_ = initialized;
    Refresh();
  }

  void RecordSave(std::uint64_t incident_id, bool succeeded) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (initialized_) {
      if (succeeded) {
        pending_incidents_.erase(incident_id);
      } else {
        pending_incidents_.insert(incident_id);
      }
    }
    Refresh();
  }

  bool IsDegraded() const { return degraded_.load(); }

 private:
  void Refresh() {
    degraded_.store(!initialized_ || !pending_incidents_.empty());
  }

  mutable std::mutex mutex_;
  std::unordered_set<std::uint64_t> pending_incidents_;
  bool initialized_ = false;
  std::atomic<bool> degraded_{true};
};

// 管理多个远程主机的监控数据（推送模式）
class HostManager {
 public:
  HostManager();
  ~HostManager();

  // 启动后台处理线程
  void Start();
  void Stop();

  // 校验并将工作者数据提交到对应 Host 分片。
  DataReceiveResult Submit(const monitor::proto::MonitorInfo& info);

  // 获取所有主机评分
  std::unordered_map<std::string, HostScore> GetAllHostScores();

  // 获取最优主机
  std::string GetBestHost();

  std::vector<diagnostics::IncidentRecord> GetIncidents(
      const std::string& server_name = {},
      std::chrono::system_clock::time_point start_time =
          std::chrono::system_clock::time_point::min(),
      std::chrono::system_clock::time_point end_time =
          std::chrono::system_clock::time_point::max(),
      const std::string& root_cause = {},
      const std::string& severity = {}) const;
  std::optional<diagnostics::IncidentRecord> GetIncident(
      std::uint64_t incident_id) const;
  std::vector<diagnostics::IncidentRecord> GetActiveIncidents(
      const std::string& server_name = {}) const;
  bool IsDiagnosticPersistenceDegraded() const {
    return diagnostic_persistence_state_.IsDegraded();
  }

 private:
  void ProcessLoop();
  void ProcessOne(std::size_t shard_id, const std::string& host_name,
                  const monitor::proto::MonitorInfo& info,
                  std::chrono::system_clock::time_point received_at,
                  std::chrono::steady_clock::time_point enqueued_at);
  void PersistTask(PersistenceTask task);
  double CalcScore(const monitor::proto::MonitorInfo& info);
  void WriteToMysql(const std::string& host_name, const HostScore& host_score,
                    double net_in_rate, double net_out_rate,
                    float cpu_percent_rate, float usr_percent_rate,
                    float system_percent_rate, float nice_percent_rate,
                    float idle_percent_rate, float io_wait_percent_rate,
                    float irq_percent_rate, float soft_irq_percent_rate,
                    float steal_percent_rate, float guest_percent_rate,
                    float guest_nice_percent_rate, float load_avg_1_rate,
                    float load_avg_3_rate, float load_avg_15_rate,
                    float mem_used_percent_rate, float mem_total_rate,
                    float mem_free_rate, float mem_avail_rate,
                    float net_in_rate_rate, float net_out_rate_rate,
                    float net_in_drop_rate_rate, float net_out_drop_rate_rate);

  std::unordered_map<std::string, HostScore> host_scores_;
  std::mutex mtx_;
  std::condition_variable process_condition_;
  std::atomic<bool> running_;
  std::unique_ptr<std::thread> thread_;
  std::unique_ptr<HostShardExecutor> shard_executor_;
  std::unique_ptr<PersistenceWorker> persistence_worker_;
  std::vector<std::unordered_map<std::string, PerfSample>> shard_perf_samples_;
  std::atomic<std::uint64_t> accepted_count_{0};
  std::atomic<std::uint64_t> queue_full_count_{0};
  std::atomic<std::uint64_t> processed_count_{0};
  std::atomic<std::uint64_t> persistence_task_count_{0};
  std::atomic<std::uint64_t> persistence_rejected_count_{0};
  std::atomic<std::uint64_t> queue_delay_samples_{0};
  std::atomic<std::uint64_t> queue_delay_total_us_{0};
  std::atomic<std::uint64_t> max_queue_delay_us_{0};
  PersistenceHistory persistence_history_;
  diagnostics::EvidenceBuilder evidence_builder_;
  diagnostics::RootCauseEngine root_cause_engine_;
  diagnostics::IncidentStore incident_store_;
  diagnostics::DiagnosticPersistence diagnostic_persistence_;
  DiagnosticPersistenceState diagnostic_persistence_state_;
};

}  // namespace monitor
