#pragma once

/**
 * @file host_manager.h
 * @brief Manager 接收 Worker MonitorInfo、计算主机评分并编排诊断持久化。
 *
 * gRPC handler 将消息提交到 HostShardExecutor；同一 host 哈希到同一
 * shard 后由 ProcessOne 顺序计算 CPU 概览、变化率、score、Evidence 和
 * RootCause，再把 PersistenceTask 投递给异步 PersistenceWorker。查询
 * 接口读取内存 IncidentStore/host_scores，不直接阻塞接收线程。
 */

#include <atomic>
#include <chrono>
#include <cstddef>
#include <condition_variable>
#include <cstdint>
#include <deque>
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
#include "health/health_score_engine.h"
#include "host_shard_executor.h"
#include "persistence_history.h"
#include "persistence_task.h"
#include "persistence_worker.h"

#include "monitor_info.pb.h"

namespace monitor {

/**
 * @brief 单个 Host 上一轮基础指标快照，用于计算变化率。
 *
 * 这些字段是 Manager 侧缓存值，不是新的 protobuf schema；rate 由当前
 * 样本与该快照比较得到，只有同一 shard 顺序处理才能保证前后关系。
 */
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

/**
 * @brief Manager 侧整机 CPU 普通监控概览。
 *
 * Worker 上报多个 CPU 核，普通概览对有效核取平均，同时记录 peak core。
 * 异常检测仍使用逐核最大值，避免单核热点被平均值稀释。
 */
struct CpuOverview {
  float cpu_percent = 0;
  float usr_percent = 0;
  float system_percent = 0;
  float nice_percent = 0;
  float idle_percent = 0;
  float io_wait_percent = 0;
  float irq_percent = 0;
  float soft_irq_percent = 0;
  std::size_t cpu_count = 0;
  std::string peak_core_name;
  float peak_cpu_percent = 0;
};

struct NetworkOverview {
  double reported_total_recv_kib_per_sec = 0;
  double reported_total_send_kib_per_sec = 0;
  double peak_recv_kib_per_sec = 0;
  double peak_send_kib_per_sec = 0;
  double total_packets_per_sec = 0;
  std::size_t interface_count = 0;
};

/** @brief 从有效逐核 CPU 样本生成平均值和最忙核心。 */
CpuOverview BuildCpuOverview(const monitor::proto::MonitorInfo& info);
NetworkOverview BuildNetworkOverview(const monitor::proto::MonitorInfo& info);

/** @brief 未配置时返回默认值；显式空值、零值或非法值返回 nullopt。 */
std::optional<std::size_t> ParsePositiveSizeConfig(
    const char* value, std::size_t default_value);

struct ScoreResult {
  double score = 0;
  bool valid = false;
};

/**
 * @brief 线程安全地暴露诊断 MySQL 初始化/待保存 incident 的降级状态。
 *
 * initialized_ 和 pending_incidents_ 由 mutex_ 保护，degraded_ 用 atomic
 * 供状态查询快速读取；它描述持久化能力，不影响内存诊断路径继续运行。
 */
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

/**
 * @brief Manager 的主机接收、评分、诊断和异步持久化总协调器。
 *
 * HostManager 自身只维护全局目录和生命周期；按 host 的顺序性由
 * HostShardExecutor 保证，数据库写入由 PersistenceWorker 解耦。
 */
class HostManager {
 public:
  explicit HostManager(
      std::chrono::milliseconds health_maintenance_interval =
          std::chrono::minutes(1),
      std::chrono::milliseconds health_max_idle = std::chrono::minutes(5));
  ~HostManager();

  /** @brief 初始化持久化/分片执行器并启动后台线程。 */
  bool Start();
  /** @brief 停止接收、等待处理队列和持久化队列有序退出。 */
  void Stop();

  /** @brief 校验主机标识并将 Worker 消息提交到对应 host 分片。 */
  DataReceiveResult Submit(const monitor::proto::MonitorInfo& info);
  DataReceiveResult SubmitWithFeedback(
      const monitor::proto::MonitorInfo& info,
      std::chrono::system_clock::time_point deadline,
      monitor::proto::MonitorFeedback* feedback);
  std::uint64_t HealthStateEvictedCount() const {
    return health_state_evicted_count_.load(std::memory_order_relaxed);
  }
  std::uint64_t ProcessedCount() const {
    return processed_count_.load(std::memory_order_relaxed);
  }

  // 获取所有主机评分
  std::unordered_map<std::string, HostScore> GetAllHostScores();

  // 获取最优主机
  std::string GetBestHost();
  std::string GetHealthiestHost();

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
  HostFeedbackResult ProcessOne(
      std::size_t shard_id, const std::string& host_name,
      const monitor::proto::MonitorInfo& info,
      std::chrono::system_clock::time_point received_at,
      std::chrono::steady_clock::time_point enqueued_at);
  void MaintainShard(std::size_t shard_id,
                     std::chrono::steady_clock::time_point now);
  bool PersistTask(PersistenceTask task);
  ScoreResult CalcResourceScore(const monitor::proto::MonitorInfo& info);
  bool WriteToMysql(const std::string& host_name, const HostScore& host_score,
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
  std::vector<std::unordered_map<std::string, health::HealthScoreEngine>>
      shard_health_engines_;
  struct FeedbackCacheEntry {
    std::uint64_t sequence = 0;
    std::int64_t timestamp_ms = 0;
    HostFeedbackResult result;
  };
  struct HostFeedbackCache {
    std::uint64_t latest_sequence = 0;
    std::int64_t latest_timestamp_ms = 0;
    std::deque<FeedbackCacheEntry> entries;
  };
  std::vector<std::unordered_map<std::string, HostFeedbackCache>>
      shard_feedback_caches_;
  const std::chrono::milliseconds health_maintenance_interval_;
  const std::chrono::milliseconds health_max_idle_;
  health::HealthConfig health_config_;
  std::atomic<std::uint64_t> health_state_evicted_count_{0};
  std::atomic<std::uint64_t> accepted_count_{0};
  std::atomic<std::uint64_t> queue_full_count_{0};
  std::atomic<std::uint64_t> processed_count_{0};
  std::atomic<std::uint64_t> persistence_task_count_{0};
  std::atomic<std::uint64_t> persistence_failed_count_{0};
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
