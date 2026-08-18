#include "host_manager.h"

#include "mysql_timeout_config.h"

#include <algorithm>
#include <cstdlib>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <thread>
#include <utility>

#ifdef ENABLE_MYSQL
#include <mysql/mysql.h>
#endif

namespace monitor {

namespace {

const char* DiagnosticStateName(monitor::proto::ObservabilityState state) {
  switch (state) {
    case monitor::proto::OBSERVABILITY_NORMAL:
      return "NORMAL";
    case monitor::proto::OBSERVABILITY_SUSPECT:
      return "SUSPECT";
    case monitor::proto::OBSERVABILITY_DIAGNOSTIC:
      return "DIAGNOSTIC";
    case monitor::proto::OBSERVABILITY_PROFILING:
      return "PROFILING";
    case monitor::proto::OBSERVABILITY_COOLDOWN:
      return "COOLDOWN";
  }
  return "UNKNOWN";
}

bool IsMetricsLogEnabled() {
  const char* value = std::getenv("MONITOR_VERBOSE_METRICS");
  return value && std::string(value) == "1";
}

bool IsDiagnosticLogEnabled() {
  const char* value = std::getenv("KERNSCOPE_DIAGNOSTIC_LOG");
  return value && std::string(value) == "1";
}

const char* GetEnvOrDefault(const char* name, const char* default_value) {
  const char* value = std::getenv(name);
  return value && value[0] != '\0' ? value : default_value;
}

std::size_t GetEnvSize(const char* name, std::size_t default_value) {
  const char* value = std::getenv(name);
  if (!value || value[0] == '\0') return default_value;
  try {
    const auto parsed = std::stoull(value);
    return parsed == 0 ? default_value : static_cast<std::size_t>(parsed);
  } catch (...) {
    return default_value;
  }
}

std::size_t DefaultShardCount() {
  const auto hardware = std::thread::hardware_concurrency();
  const auto half = hardware == 0 ? 2u : std::max(2u, hardware / 2);
  return std::min<std::size_t>(half, 8);
}

std::string HostNameForInfo(const monitor::proto::MonitorInfo& info) {
  std::string host_name;
  if (info.has_host_info()) {
    const auto& host_info = info.host_info();
    const std::string& hostname = host_info.hostname();
    const std::string& ip = host_info.ip_address();
    if (!hostname.empty() && !ip.empty()) {
      host_name = hostname + "_" + ip;
    } else if (!hostname.empty()) {
      host_name = hostname;
    } else if (!ip.empty()) {
      host_name = ip;
    }
  }
  if (host_name.empty()) {
    host_name = info.name();
  }
  return host_name;
}

}  // namespace

#ifdef ENABLE_MYSQL
namespace {
const char* MYSQL_HOST = "127.0.0.1";
const char* MYSQL_USER = "monitor";
const char* MYSQL_PASS = "monitor123";
const char* MYSQL_DB = "monitor_db";

MYSQL* mysql_conn = nullptr;

MYSQL* GetMysqlConnection() {
  if (mysql_conn) return mysql_conn;

  mysql_conn = mysql_init(nullptr);
  if (!mysql_conn) {
    std::cerr << "mysql_init failed\n";
    return nullptr;
  }
  if (!ApplyMysqlTimeouts(mysql_conn, GetMysqlTimeoutConfig(),
                          "HostManager legacy MySQL")) {
    mysql_close(mysql_conn);
    mysql_conn = nullptr;
    return nullptr;
  }
  if (!mysql_real_connect(
          mysql_conn, GetEnvOrDefault("MONITOR_MYSQL_HOST", MYSQL_HOST),
          GetEnvOrDefault("MONITOR_MYSQL_USER", MYSQL_USER),
          GetEnvOrDefault("MONITOR_MYSQL_PASSWORD", MYSQL_PASS),
          GetEnvOrDefault("MONITOR_MYSQL_DATABASE", MYSQL_DB), 0, nullptr, 0)) {
    std::cerr << "mysql_real_connect failed: " << mysql_error(mysql_conn)
              << "\n";
    mysql_close(mysql_conn);
    mysql_conn = nullptr;
  }
  return mysql_conn;
}

void CloseMysqlConnection() {
  if (mysql_conn) {
    mysql_close(mysql_conn);
    mysql_conn = nullptr;
  }
}

}  // namespace
#endif

HostManager::HostManager() : running_(false) {}

HostManager::~HostManager() {
  Stop();
#ifdef ENABLE_MYSQL
  CloseMysqlConnection();
#endif
}

void HostManager::Start() {
  bool expected = false;
  if (!running_.compare_exchange_strong(expected, true)) {
    return;
  }
#ifdef ENABLE_MYSQL
  const bool persistence_initialized = diagnostic_persistence_.Init(
      GetEnvOrDefault("MONITOR_MYSQL_HOST", MYSQL_HOST),
      GetEnvOrDefault("MONITOR_MYSQL_USER", MYSQL_USER),
      GetEnvOrDefault("MONITOR_MYSQL_PASSWORD", MYSQL_PASS),
      GetEnvOrDefault("MONITOR_MYSQL_DATABASE", MYSQL_DB));
  diagnostic_persistence_state_.SetInitialized(persistence_initialized);
#else
  diagnostic_persistence_state_.SetInitialized(false);
#endif

  const std::size_t shard_count = GetEnvSize(
      "KERNSCOPE_MANAGER_SHARDS", DefaultShardCount());
  const std::size_t shard_queue_capacity =
      GetEnvSize("KERNSCOPE_SHARD_QUEUE_CAPACITY", 256);
  const std::size_t shard_queue_max_bytes = GetEnvSize(
      "KERNSCOPE_SHARD_QUEUE_MAX_BYTES", 64ull * 1024ull * 1024ull);
  const std::size_t persistence_queue_capacity =
      GetEnvSize("KERNSCOPE_PERSIST_QUEUE_CAPACITY", 1024);
  const std::size_t persistence_queue_max_bytes = GetEnvSize(
      "KERNSCOPE_PERSIST_QUEUE_MAX_BYTES", 128ull * 1024ull * 1024ull);
  shard_perf_samples_.clear();
  shard_perf_samples_.resize(shard_count);

  persistence_worker_ = std::make_unique<PersistenceWorker>(
      persistence_queue_capacity, persistence_queue_max_bytes,
      [this](PersistenceTask&& task) { PersistTask(std::move(task)); });
  persistence_worker_->Start();

  shard_executor_ = std::make_unique<HostShardExecutor>(
      shard_count, shard_queue_capacity, shard_queue_max_bytes,
      [this](std::size_t shard_id, const std::string& host_name,
             const monitor::proto::MonitorInfo& info,
             std::chrono::system_clock::time_point received_at,
             std::chrono::steady_clock::time_point enqueued_at) {
        ProcessOne(shard_id, host_name, info, received_at, enqueued_at);
      });
  shard_executor_->Start();
  thread_ = std::make_unique<std::thread>(&HostManager::ProcessLoop, this);
}

void HostManager::Stop() {
  if (!running_.exchange(false, std::memory_order_acq_rel)) {
    return;
  }
  process_condition_.notify_all();
  if (thread_ && thread_->joinable()) {
    thread_->join();
  }
  if (shard_executor_) {
    shard_executor_->Stop();
  }
  if (persistence_worker_) {
    persistence_worker_->Stop();
  }
  std::cout << "[KernScopeManager] processing_stats accepted="
            << accepted_count_.load(std::memory_order_relaxed)
            << " queue_full="
            << queue_full_count_.load(std::memory_order_relaxed)
            << " processed=" << processed_count_.load(std::memory_order_relaxed)
            << " persistence_tasks="
            << persistence_task_count_.load(std::memory_order_relaxed)
            << " persistence_rejected="
            << persistence_rejected_count_.load(std::memory_order_relaxed)
            << " queue_delay_samples="
            << queue_delay_samples_.load(std::memory_order_relaxed)
            << " queue_delay_total_us="
            << queue_delay_total_us_.load(std::memory_order_relaxed)
            << " max_queue_delay_us="
            << max_queue_delay_us_.load(std::memory_order_relaxed)
            << " max_shard_queue_depth="
            << (shard_executor_ ? shard_executor_->PeakQueueDepth() : 0)
            << " max_shard_queue_bytes="
            << (shard_executor_ ? shard_executor_->PeakQueueBytes() : 0)
            << " max_persistence_queue_depth="
            << (persistence_worker_ ? persistence_worker_->PeakQueueDepth()
                                     : 0)
            << " max_persistence_queue_bytes="
            << (persistence_worker_ ? persistence_worker_->PeakQueueBytes()
                                     : 0)
            << std::endl;
}

void HostManager::ProcessLoop() {
  while (running_) {
    std::unique_lock<std::mutex> wait_lock(mtx_);
    if (process_condition_.wait_for(wait_lock, std::chrono::seconds(60),
                                    [this] { return !running_.load(); })) {
      break;
    }

    // 获取当前时间
    auto now = std::chrono::system_clock::now();
    // 遍历 host_scores_，移除过期的主机数据
    for (auto it = host_scores_.begin(); it != host_scores_.end();) {
      auto age = std::chrono::duration_cast<std::chrono::seconds>(
                     now - it->second.timestamp)
                     .count();
      // 如果某个主机的最新数据超过 60 秒没有更新，就认为它已经不活跃了，从
      // host_scores_ 中移除
      if (age > 60) {
        std::cout << "Removing stale host: " << it->first << std::endl;
        it = host_scores_.erase(it);  // erase 返回下一个迭代器
      } else {
        ++it;
      }
    }
  }
}

DataReceiveResult HostManager::Submit(
    const monitor::proto::MonitorInfo& info) {
  const std::string host_name = HostNameForInfo(info);
  if (host_name.empty()) {
    std::cerr << "Received data with empty server identifier" << std::endl;
    return DataReceiveResult::kInvalidHost;
  }
  if (!shard_executor_) {
    return DataReceiveResult::kStopping;
  }
  const auto result = shard_executor_->Submit(host_name, info);
  if (result == DataReceiveResult::kAccepted) {
    accepted_count_.fetch_add(1, std::memory_order_relaxed);
  } else if (result == DataReceiveResult::kQueueFull) {
    queue_full_count_.fetch_add(1, std::memory_order_relaxed);
  }
  return result;
}

// 计算主机的综合评分
void HostManager::ProcessOne(
    std::size_t shard_id, const std::string& host_name,
    const monitor::proto::MonitorInfo& info,
    std::chrono::system_clock::time_point received_at,
    std::chrono::steady_clock::time_point enqueued_at) {

  double score = CalcScore(info);
  const auto now = received_at;
  const auto queue_delay = std::chrono::steady_clock::now() - enqueued_at;
  const auto queue_delay_us = static_cast<std::uint64_t>(std::max<std::int64_t>(
      0, std::chrono::duration_cast<std::chrono::microseconds>(queue_delay)
             .count()));
  queue_delay_samples_.fetch_add(1, std::memory_order_relaxed);
  queue_delay_total_us_.fetch_add(queue_delay_us, std::memory_order_relaxed);
  auto current_max = max_queue_delay_us_.load(std::memory_order_relaxed);
  while (current_max < queue_delay_us &&
         !max_queue_delay_us_.compare_exchange_weak(
             current_max, queue_delay_us, std::memory_order_relaxed,
             std::memory_order_relaxed)) {
  }

  // 网络速率计算
  double net_in_rate = 0, net_out_rate = 0;
  if (info.net_info_size() > 0) {
    net_in_rate = info.net_info(0).rcv_rate() / (1024.0 * 1024.0);
    net_out_rate = info.net_info(0).send_rate() / (1024.0 * 1024.0);
  }

  // 当前采样
  PerfSample curr;
  if (info.cpu_stat_size() > 0) {
    const auto& cpu = info.cpu_stat(0);
    curr.cpu_percent = cpu.cpu_percent();
    curr.usr_percent = cpu.usr_percent();
    curr.system_percent = cpu.system_percent();
    curr.nice_percent = cpu.nice_percent();
    curr.idle_percent = cpu.idle_percent();
    curr.io_wait_percent = cpu.io_wait_percent();
    curr.irq_percent = cpu.irq_percent();
    curr.soft_irq_percent = cpu.soft_irq_percent();
  }
  if (info.has_cpu_load()) {
    curr.load_avg_1 = info.cpu_load().load_avg_1();
    curr.load_avg_3 = info.cpu_load().load_avg_3();
    curr.load_avg_15 = info.cpu_load().load_avg_15();
  }
  if (info.has_mem_info()) {
    curr.mem_used_percent = info.mem_info().used_percent();
    curr.mem_total = info.mem_info().total();
    curr.mem_free = info.mem_info().free();
    curr.mem_avail = info.mem_info().avail();
  }
  curr.net_in_rate = net_in_rate;
  curr.net_out_rate = net_out_rate;
  curr.score = score;

  // 变化率计算：同一 Host 始终由同一个 shard worker 访问该状态。
  auto& last = shard_perf_samples_[shard_id][host_name];
  auto rate = [](float now_val, float last_val) -> float {
    if (last_val == 0) return 0;
    return (now_val - last_val) / last_val;
  };

  PersistenceTask task;
  task.host_name = host_name;
  task.host_score = HostScore{info, score, now};
  task.net_in_rate = net_in_rate;
  task.net_out_rate = net_out_rate;
  task.cpu_percent_rate = rate(curr.cpu_percent, last.cpu_percent);
  task.usr_percent_rate = rate(curr.usr_percent, last.usr_percent);
  task.system_percent_rate = rate(curr.system_percent, last.system_percent);
  task.nice_percent_rate = rate(curr.nice_percent, last.nice_percent);
  task.idle_percent_rate = rate(curr.idle_percent, last.idle_percent);
  task.io_wait_percent_rate = rate(curr.io_wait_percent, last.io_wait_percent);
  task.irq_percent_rate = rate(curr.irq_percent, last.irq_percent);
  task.soft_irq_percent_rate = rate(curr.soft_irq_percent, last.soft_irq_percent);
  task.load_avg_1_rate = rate(curr.load_avg_1, last.load_avg_1);
  task.load_avg_3_rate = rate(curr.load_avg_3, last.load_avg_3);
  task.load_avg_15_rate = rate(curr.load_avg_15, last.load_avg_15);
  task.mem_used_percent_rate =
      rate(curr.mem_used_percent, last.mem_used_percent);
  task.mem_total_rate = rate(curr.mem_total, last.mem_total);
  task.mem_free_rate = rate(curr.mem_free, last.mem_free);
  task.mem_avail_rate = rate(curr.mem_avail, last.mem_avail);
  task.net_in_rate_rate = rate(curr.net_in_rate, last.net_in_rate);
  task.net_out_rate_rate = rate(curr.net_out_rate, last.net_out_rate);
  last = curr;

  {
    std::lock_guard<std::mutex> lock(mtx_);
    host_scores_[host_name] = task.host_score;
  }

  if (info.has_diagnostic()) {
    const auto evidence = evidence_builder_.Build(info, now);
    const auto root_causes = root_cause_engine_.Evaluate(evidence);
    if (IsDiagnosticLogEnabled()) {
      std::cout << "[KernScopeManager] state="
                << DiagnosticStateName(info.diagnostic().state())
                << " evidence=";
      for (std::size_t index = 0; index < evidence.size(); ++index) {
        if (index != 0) std::cout << ",";
        std::cout << diagnostics::EvidenceTypeName(evidence[index].type)
                  << ":severity=" << evidence[index].severity;
      }
      std::cout << " root_causes=";
      for (std::size_t index = 0; index < root_causes.size(); ++index) {
        if (index != 0) std::cout << ",";
        std::cout << diagnostics::RootCauseTypeName(root_causes[index].type)
                  << ":confidence=" << root_causes[index].confidence;
      }
      std::cout << std::endl;
    }
    task.incident = incident_store_.Observe(
        host_name, DiagnosticStateName(info.diagnostic().state()), evidence,
        root_causes, now);
  }

  if (IsMetricsLogEnabled()) {
    std::cout << "\n================== Received Data =================="
              << std::endl;
    std::cout << "Server: " << host_name << ", Score: " << score << std::endl;

    // CPU 详细信息
    std::cout << "\n--- CPU ---" << std::endl;
    std::cout << "  Usage: " << curr.cpu_percent << "%, "
              << "User: " << curr.usr_percent << "%, "
              << "System: " << curr.system_percent << "%" << std::endl;
    std::cout << "  Nice: " << curr.nice_percent << "%, "
              << "Idle: " << curr.idle_percent << "%, "
              << "IOWait: " << curr.io_wait_percent << "%" << std::endl;
    std::cout << "  IRQ: " << curr.irq_percent << "%, "
              << "SoftIRQ: " << curr.soft_irq_percent << "%" << std::endl;
    std::cout << "  Load: " << curr.load_avg_1 << "/" << curr.load_avg_3 << "/"
              << curr.load_avg_15 << std::endl;

    // 内存详细信息
    std::cout << "\n--- Memory ---" << std::endl;
    std::cout << "  Used: " << curr.mem_used_percent << "%, "
              << "Total: " << curr.mem_total << " MB" << std::endl;
    std::cout << "  Free: " << curr.mem_free << " MB, "
              << "Avail: " << curr.mem_avail << " MB" << std::endl;

    // 网络详细信息
    std::cout << "\n--- Network ---" << std::endl;
    std::cout << "  In: " << net_in_rate * 1024 * 1024 << " B/s, "
              << "Out: " << net_out_rate * 1024 * 1024 << " B/s" << std::endl;
    for (int i = 0; i < info.net_info_size(); ++i) {
      const auto& net = info.net_info(i);
      std::cout << "  [" << net.name() << "] Recv: " << net.rcv_rate()
                << " B/s, "
                << "Send: " << net.send_rate() << " B/s, "
                << "Drops: " << net.drop_in() << "/" << net.drop_out()
                << std::endl;
    }

    // 磁盘详细信息
    std::cout << "\n--- Disk ---" << std::endl;
    float max_disk_util = 0;
    for (int i = 0; i < info.disk_info_size(); ++i) {
      const auto& disk = info.disk_info(i);
      std::cout << "  [" << disk.name() << "] "
                << "Read: " << disk.read_bytes_per_sec() / 1024.0 << " KB/s, "
                << "Write: " << disk.write_bytes_per_sec() / 1024.0 << " KB/s, "
                << "Util: " << disk.util_percent() << "%" << std::endl;
      if (disk.util_percent() > max_disk_util)
        max_disk_util = disk.util_percent();
    }
    if (info.disk_info_size() == 0) {
      std::cout << "  No disk data" << std::endl;
    }

    // 软中断信息
    std::cout << "\n--- SoftIRQ ---" << std::endl;
    std::cout << "  CPU cores with softirq data: " << info.soft_irq_size()
              << std::endl;

    // 变化率信息
    std::cout << "\n--- Change Rates ---" << std::endl;
    std::cout << "  CPU: " << task.cpu_percent_rate * 100 << "%, "
              << "Mem: " << task.mem_used_percent_rate * 100 << "%, "
              << "Load: " << task.load_avg_1_rate * 100 << "%" << std::endl;
    std::cout << "  NetIn: " << task.net_in_rate_rate * 100 << "%, "
              << "NetOut: " << task.net_out_rate_rate * 100 << "%" << std::endl;

    std::cout << "====================================================\n"
              << std::endl;
  }

  // processed means ProcessOne completed and the PersistenceTask was accepted
  // by the PersistenceWorker; it does not mean that MySQL has completed.
  if (persistence_worker_ && persistence_worker_->Enqueue(std::move(task))) {
    processed_count_.fetch_add(1, std::memory_order_relaxed);
  } else {
    persistence_rejected_count_.fetch_add(1, std::memory_order_relaxed);
    std::cerr << "ERROR: persistence task rejected (worker stopped, queue "
                 "closed, or task exceeded the byte budget)"
              << std::endl;
  }
}

void HostManager::PersistTask(PersistenceTask task) {
  // Only the single PersistenceWorker thread calls this method. The legacy
  // WriteToMysql detail-rate history therefore has one owner and no mutex.
  persistence_task_count_.fetch_add(1, std::memory_order_relaxed);
  if (task.incident) {
    const bool persisted = diagnostic_persistence_.Save(*task.incident);
    diagnostic_persistence_state_.RecordSave(task.incident->id, persisted);
    if (!persisted) {
      std::cerr << "ERROR: diagnostic persistence degraded; incident "
                << task.incident->id << " remains available in memory"
                << std::endl;
    }
  }

  WriteToMysql(task.host_name, task.host_score, task.net_in_rate,
               task.net_out_rate, task.cpu_percent_rate, task.usr_percent_rate,
               task.system_percent_rate, task.nice_percent_rate,
               task.idle_percent_rate, task.io_wait_percent_rate,
               task.irq_percent_rate, task.soft_irq_percent_rate,
               task.steal_percent_rate, task.guest_percent_rate,
               task.guest_nice_percent_rate, task.load_avg_1_rate,
               task.load_avg_3_rate, task.load_avg_15_rate,
               task.mem_used_percent_rate, task.mem_total_rate,
               task.mem_free_rate, task.mem_avail_rate, task.net_in_rate_rate,
               task.net_out_rate_rate, task.net_in_drop_rate_rate,
               task.net_out_drop_rate_rate);
}

std::unordered_map<std::string, HostScore> HostManager::GetAllHostScores() {
  std::lock_guard<std::mutex> lock(mtx_);
  return host_scores_;
}

std::string HostManager::GetBestHost() {
  std::lock_guard<std::mutex> lock(mtx_);
  std::string best_host;
  double best_score = -1;
  for (const auto& [host, data] : host_scores_) {
    if (data.score > best_score) {
      best_score = data.score;
      best_host = host;
    }
  }
  return best_host;
}

std::vector<diagnostics::IncidentRecord> HostManager::GetIncidents(
    const std::string& server_name,
    std::chrono::system_clock::time_point start_time,
    std::chrono::system_clock::time_point end_time,
    const std::string& root_cause, const std::string& severity) const {
  return incident_store_.List(server_name, start_time, end_time, root_cause,
                              severity);
}

std::optional<diagnostics::IncidentRecord> HostManager::GetIncident(
    std::uint64_t incident_id) const {
  return incident_store_.Get(incident_id);
}

std::vector<diagnostics::IncidentRecord> HostManager::GetActiveIncidents(
    const std::string& server_name) const {
  return incident_store_.Active(server_name);
}

double HostManager::CalcScore(const monitor::proto::MonitorInfo& info) {
  // ============================================================
  // 性能评分模型 - 针对学校选课/查成绩系统高并发场景优化
  // ============================================================
  // 权重配置：
  // - CPU 使用率: 35%
  // - 内存使用率: 30%
  // - CPU 负载:   15%
  // - 磁盘 IO:    15%
  // - 网络带宽:    5% (收发各 2.5%)
  // ============================================================

  const double cpu_weight = 0.35;
  const double mem_weight = 0.30;
  const double load_weight = 0.15;
  const double disk_weight = 0.15;
  const double net_weight = 0.05;

  const double load_coefficient = 1.5;       // I/O 密集型场景系数
  const double max_bandwidth = 125000000.0;  // 1Gbps

  double cpu_percent = 0, load_avg_1 = 0, mem_percent = 0;
  double net_recv_rate = 0, net_send_rate = 0, disk_util = 0;
  int cpu_cores = 1;

  if (info.cpu_stat_size() > 0) {
    cpu_percent = info.cpu_stat(0).cpu_percent();
    cpu_cores = info.cpu_stat_size() - 1;
    if (cpu_cores < 1) cpu_cores = 1;
  }
  if (info.has_cpu_load()) {
    load_avg_1 = info.cpu_load().load_avg_1();
  }
  if (info.has_mem_info()) {
    mem_percent = info.mem_info().used_percent();
  }
  if (info.net_info_size() > 0) {
    net_recv_rate = info.net_info(0).rcv_rate();
    net_send_rate = info.net_info(0).send_rate();
  }
  if (info.disk_info_size() > 0) {
    for (int i = 0; i < info.disk_info_size(); ++i) {
      double util = info.disk_info(i).util_percent();
      if (util > disk_util) disk_util = util;
    }
  }

  // 反向归一化
  auto clamp = [](double v) { return v < 0 ? 0 : (v > 1 ? 1 : v); };

  double cpu_score = clamp(1.0 - cpu_percent / 100.0);
  double mem_score = clamp(1.0 - mem_percent / 100.0);
  double load_score = clamp(1.0 - load_avg_1 / (cpu_cores * load_coefficient));
  double disk_score = clamp(1.0 - disk_util / 100.0);
  double net_recv_score = clamp(1.0 - net_recv_rate / max_bandwidth);
  double net_send_score = clamp(1.0 - net_send_rate / max_bandwidth);
  double net_score = (net_recv_score + net_send_score) / 2.0;

  double score = cpu_score * cpu_weight + mem_score * mem_weight +
                 load_score * load_weight + disk_score * disk_weight +
                 net_score * net_weight;

  score *= 100.0;
  return score < 0 ? 0 : (score > 100 ? 100 : score);
}

void HostManager::WriteToMysql(
    const std::string& host_name, const HostScore& host_score,
    double net_in_rate, double net_out_rate, float cpu_percent_rate,
    float usr_percent_rate, float system_percent_rate, float nice_percent_rate,
    float idle_percent_rate, float io_wait_percent_rate, float irq_percent_rate,
    float soft_irq_percent_rate, float steal_percent_rate,
    float guest_percent_rate, float guest_nice_percent_rate,
    float load_avg_1_rate, float load_avg_3_rate, float load_avg_15_rate,
    float mem_used_percent_rate, float mem_total_rate, float mem_free_rate,
    float mem_avail_rate, float net_in_rate_rate, float net_out_rate_rate,
    float net_in_drop_rate_rate, float net_out_drop_rate_rate) {
#ifdef ENABLE_MYSQL
  MYSQL* conn = GetMysqlConnection();
  if (!conn) return;

  // 时间戳
  std::time_t t = std::chrono::system_clock::to_time_t(host_score.timestamp);
  std::tm tm_time;
  localtime_r(&t, &tm_time);
  char time_buf[32];
  std::strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", &tm_time);

  const auto& info = host_score.info;
  auto rate = [](float now_val, float last_val) -> float {
    if (last_val == 0) return 0;
    return (now_val - last_val) / last_val;
  };

  // ========== 1. 写入主表 server_performance ==========
  {
    float total = 0, free_mem = 0, avail = 0, send_rate = 0, rcv_rate = 0;
    float cpu_percent = 0, usr_percent = 0, system_percent = 0;
    float nice_percent = 0, idle_percent = 0, io_wait_percent = 0;
    float irq_percent = 0, soft_irq_percent = 0;
    float load_avg_1 = 0, load_avg_3 = 0, load_avg_15 = 0, mem_used_percent = 0;
    float disk_util_percent = 0;

    if (info.has_mem_info()) {
      total = info.mem_info().total();
      free_mem = info.mem_info().free();
      avail = info.mem_info().avail();
      mem_used_percent = info.mem_info().used_percent();
    }
    if (info.net_info_size() > 0) {
      send_rate = info.net_info(0).send_rate() / 1024.0;
      rcv_rate = info.net_info(0).rcv_rate() / 1024.0;
    }
    if (info.cpu_stat_size() > 0) {
      const auto& cpu = info.cpu_stat(0);
      cpu_percent = cpu.cpu_percent();
      usr_percent = cpu.usr_percent();
      system_percent = cpu.system_percent();
      nice_percent = cpu.nice_percent();
      idle_percent = cpu.idle_percent();
      io_wait_percent = cpu.io_wait_percent();
      irq_percent = cpu.irq_percent();
      soft_irq_percent = cpu.soft_irq_percent();
    }
    if (info.has_cpu_load()) {
      load_avg_1 = info.cpu_load().load_avg_1();
      load_avg_3 = info.cpu_load().load_avg_3();
      load_avg_15 = info.cpu_load().load_avg_15();
    }
    // 获取磁盘利用率最大值（用于评分）
    for (int i = 0; i < info.disk_info_size(); ++i) {
      float util = info.disk_info(i).util_percent();
      if (util > disk_util_percent) disk_util_percent = util;
    }

    // 计算磁盘利用率变化率
    float disk_util_percent_rate = 0;
    if (persistence_history_.disk_util.count(host_name) &&
        persistence_history_.disk_util[host_name] != 0) {
      disk_util_percent_rate =
          (disk_util_percent - persistence_history_.disk_util[host_name]) /
          persistence_history_.disk_util[host_name];
    }
    persistence_history_.disk_util[host_name] = disk_util_percent;

    std::ostringstream oss;
    oss << "INSERT INTO server_performance "
        << "(server_name, cpu_percent, usr_percent, system_percent, "
           "nice_percent, "
        << "idle_percent, io_wait_percent, irq_percent, soft_irq_percent, "
        << "load_avg_1, load_avg_3, load_avg_15, "
        << "mem_used_percent, total, free, avail, "
        << "disk_util_percent, send_rate, rcv_rate, score, "
        << "cpu_percent_rate, usr_percent_rate, system_percent_rate, "
        << "nice_percent_rate, idle_percent_rate, io_wait_percent_rate, "
        << "irq_percent_rate, soft_irq_percent_rate, "
        << "load_avg_1_rate, load_avg_3_rate, load_avg_15_rate, "
        << "mem_used_percent_rate, total_rate, free_rate, avail_rate, "
        << "disk_util_percent_rate, send_rate_rate, rcv_rate_rate, timestamp) "
           "VALUES ('"
        << host_name << "'," << cpu_percent << "," << usr_percent << ","
        << system_percent << "," << nice_percent << "," << idle_percent << ","
        << io_wait_percent << "," << irq_percent << "," << soft_irq_percent
        << "," << load_avg_1 << "," << load_avg_3 << "," << load_avg_15 << ","
        << mem_used_percent << "," << total << "," << free_mem << "," << avail
        << "," << disk_util_percent << "," << send_rate << "," << rcv_rate
        << "," << host_score.score << "," << cpu_percent_rate << ","
        << usr_percent_rate << "," << system_percent_rate << ","
        << nice_percent_rate << "," << idle_percent_rate << ","
        << io_wait_percent_rate << "," << irq_percent_rate << ","
        << soft_irq_percent_rate << "," << load_avg_1_rate << ","
        << load_avg_3_rate << "," << load_avg_15_rate << ","
        << mem_used_percent_rate << "," << mem_total_rate << ","
        << mem_free_rate << "," << mem_avail_rate << ","
        << disk_util_percent_rate << "," << net_in_rate_rate << ","
        << net_out_rate_rate << ",'" << time_buf << "')";
    mysql_query(conn, oss.str().c_str());
  }

  // ========== 2. 写入网络详细表 server_net_detail ==========
  for (int i = 0; i < info.net_info_size(); ++i) {
    const auto& net = info.net_info(i);
    std::string net_name = net.name();

    NetDetailSample curr;
    curr.rcv_bytes_rate = net.rcv_rate();
    curr.rcv_packets_rate = net.rcv_packets_rate();
    curr.snd_bytes_rate = net.send_rate();
    curr.snd_packets_rate = net.send_packets_rate();
    curr.err_in = net.err_in();
    curr.err_out = net.err_out();
    curr.drop_in = net.drop_in();
    curr.drop_out = net.drop_out();

    NetDetailSample& last =
        persistence_history_.net_samples[host_name][net_name];

    // 计算错误/丢弃变化率
    auto rate_u64 = [](uint64_t now_val, uint64_t last_val) -> float {
      if (last_val == 0) return 0;
      return static_cast<float>(now_val - last_val) /
             static_cast<float>(last_val);
    };

    std::ostringstream oss;
    oss << "INSERT INTO server_net_detail "
        << "(server_name, net_name, err_in, err_out, drop_in, drop_out, "
        << "rcv_bytes_rate, rcv_packets_rate, snd_bytes_rate, "
           "snd_packets_rate, "
        << "rcv_bytes_rate_rate, rcv_packets_rate_rate, "
        << "snd_bytes_rate_rate, snd_packets_rate_rate, "
        << "err_in_rate, err_out_rate, drop_in_rate, drop_out_rate, "
        << "timestamp) VALUES ('" << host_name << "','" << net_name << "',"
        << curr.err_in << "," << curr.err_out << "," << curr.drop_in << ","
        << curr.drop_out << "," << curr.rcv_bytes_rate << ","
        << curr.rcv_packets_rate << "," << curr.snd_bytes_rate << ","
        << curr.snd_packets_rate << ","
        << rate(curr.rcv_bytes_rate, last.rcv_bytes_rate) << ","
        << rate(curr.rcv_packets_rate, last.rcv_packets_rate) << ","
        << rate(curr.snd_bytes_rate, last.snd_bytes_rate) << ","
        << rate(curr.snd_packets_rate, last.snd_packets_rate) << ","
        << rate_u64(curr.err_in, last.err_in) << ","
        << rate_u64(curr.err_out, last.err_out) << ","
        << rate_u64(curr.drop_in, last.drop_in) << ","
        << rate_u64(curr.drop_out, last.drop_out) << ",'" << time_buf << "')";
    mysql_query(conn, oss.str().c_str());

    last = curr;
  }

  // ========== 3. 写入软中断详细表 server_softirq_detail ==========
  for (int i = 0; i < info.soft_irq_size(); ++i) {
    const auto& sirq = info.soft_irq(i);
    std::string cpu_name = sirq.cpu();

    SoftIrqSample curr;
    curr.hi = sirq.hi();
    curr.timer = sirq.timer();
    curr.net_tx = sirq.net_tx();
    curr.net_rx = sirq.net_rx();
    curr.block = sirq.block();
    curr.irq_poll = sirq.irq_poll();
    curr.tasklet = sirq.tasklet();
    curr.sched = sirq.sched();
    curr.hrtimer = sirq.hrtimer();
    curr.rcu = sirq.rcu();

    SoftIrqSample& last =
        persistence_history_.softirq_samples[host_name][cpu_name];

    std::ostringstream oss;
    oss << "INSERT INTO server_softirq_detail "
        << "(server_name, cpu_name, hi, timer, net_tx, net_rx, block, "
        << "irq_poll, tasklet, sched, hrtimer, rcu, "
        << "hi_rate, timer_rate, net_tx_rate, net_rx_rate, block_rate, "
        << "irq_poll_rate, tasklet_rate, sched_rate, hrtimer_rate, rcu_rate, "
        << "timestamp) VALUES ('" << host_name << "','" << cpu_name << "',"
        << curr.hi << "," << curr.timer << "," << curr.net_tx << ","
        << curr.net_rx << "," << curr.block << "," << curr.irq_poll << ","
        << curr.tasklet << "," << curr.sched << "," << curr.hrtimer << ","
        << curr.rcu << "," << rate(curr.hi, last.hi) << ","
        << rate(curr.timer, last.timer) << "," << rate(curr.net_tx, last.net_tx)
        << "," << rate(curr.net_rx, last.net_rx) << ","
        << rate(curr.block, last.block) << ","
        << rate(curr.irq_poll, last.irq_poll) << ","
        << rate(curr.tasklet, last.tasklet) << ","
        << rate(curr.sched, last.sched) << ","
        << rate(curr.hrtimer, last.hrtimer) << "," << rate(curr.rcu, last.rcu)
        << ",'" << time_buf << "')";
    mysql_query(conn, oss.str().c_str());

    last = curr;
  }

  // ========== 4. 写入内存详细表 server_mem_detail ==========
  if (info.has_mem_info()) {
    const auto& mem = info.mem_info();

    MemDetailSample curr;
    curr.total = mem.total();
    curr.free = mem.free();
    curr.avail = mem.avail();
    curr.buffers = mem.buffers();
    curr.cached = mem.cached();
    curr.swap_cached = mem.swap_cached();
    curr.active = mem.active();
    curr.inactive = mem.inactive();
    curr.active_anon = mem.active_anon();
    curr.inactive_anon = mem.inactive_anon();
    curr.active_file = mem.active_file();
    curr.inactive_file = mem.inactive_file();
    curr.dirty = mem.dirty();
    curr.writeback = mem.writeback();
    curr.anon_pages = mem.anon_pages();
    curr.mapped = mem.mapped();
    curr.kreclaimable = mem.kreclaimable();
    curr.sreclaimable = mem.sreclaimable();
    curr.sunreclaim = mem.sunreclaim();

    MemDetailSample& last = persistence_history_.mem_samples[host_name];

    std::ostringstream oss;
    oss << "INSERT INTO server_mem_detail "
        << "(server_name, total, free, avail, buffers, cached, swap_cached, "
        << "active, inactive, active_anon, inactive_anon, active_file, "
           "inactive_file, "
        << "dirty, writeback, anon_pages, mapped, kreclaimable, sreclaimable, "
           "sunreclaim, "
        << "total_rate, free_rate, avail_rate, buffers_rate, cached_rate, "
           "swap_cached_rate, "
        << "active_rate, inactive_rate, active_anon_rate, inactive_anon_rate, "
        << "active_file_rate, inactive_file_rate, dirty_rate, writeback_rate, "
        << "anon_pages_rate, mapped_rate, kreclaimable_rate, "
           "sreclaimable_rate, "
        << "sunreclaim_rate, timestamp) VALUES ('" << host_name << "',"
        << curr.total << "," << curr.free << "," << curr.avail << ","
        << curr.buffers << "," << curr.cached << "," << curr.swap_cached << ","
        << curr.active << "," << curr.inactive << "," << curr.active_anon << ","
        << curr.inactive_anon << "," << curr.active_file << ","
        << curr.inactive_file << "," << curr.dirty << "," << curr.writeback
        << "," << curr.anon_pages << "," << curr.mapped << ","
        << curr.kreclaimable << "," << curr.sreclaimable << ","
        << curr.sunreclaim << "," << rate(curr.total, last.total) << ","
        << rate(curr.free, last.free) << "," << rate(curr.avail, last.avail)
        << "," << rate(curr.buffers, last.buffers) << ","
        << rate(curr.cached, last.cached) << ","
        << rate(curr.swap_cached, last.swap_cached) << ","
        << rate(curr.active, last.active) << ","
        << rate(curr.inactive, last.inactive) << ","
        << rate(curr.active_anon, last.active_anon) << ","
        << rate(curr.inactive_anon, last.inactive_anon) << ","
        << rate(curr.active_file, last.active_file) << ","
        << rate(curr.inactive_file, last.inactive_file) << ","
        << rate(curr.dirty, last.dirty) << ","
        << rate(curr.writeback, last.writeback) << ","
        << rate(curr.anon_pages, last.anon_pages) << ","
        << rate(curr.mapped, last.mapped) << ","
        << rate(curr.kreclaimable, last.kreclaimable) << ","
        << rate(curr.sreclaimable, last.sreclaimable) << ","
        << rate(curr.sunreclaim, last.sunreclaim) << ",'" << time_buf << "')";
    mysql_query(conn, oss.str().c_str());

    last = curr;
  }

  // ========== 5. 写入磁盘详细表 server_disk_detail ==========
  for (int i = 0; i < info.disk_info_size(); ++i) {
    const auto& disk = info.disk_info(i);
    std::string disk_name = disk.name();

    DiskDetailSample curr;
    curr.read_bytes_per_sec = disk.read_bytes_per_sec();
    curr.write_bytes_per_sec = disk.write_bytes_per_sec();
    curr.read_iops = disk.read_iops();
    curr.write_iops = disk.write_iops();
    curr.avg_read_latency_ms = disk.avg_read_latency_ms();
    curr.avg_write_latency_ms = disk.avg_write_latency_ms();
    curr.util_percent = disk.util_percent();

    DiskDetailSample& last =
        persistence_history_.disk_samples[host_name][disk_name];

    std::ostringstream oss;
    oss << "INSERT INTO server_disk_detail "
        << "(server_name, disk_name, reads, writes, sectors_read, "
           "sectors_written, "
        << "read_time_ms, write_time_ms, io_in_progress, io_time_ms, "
           "weighted_io_time_ms, "
        << "read_bytes_per_sec, write_bytes_per_sec, read_iops, write_iops, "
        << "avg_read_latency_ms, avg_write_latency_ms, util_percent, "
        << "read_bytes_per_sec_rate, write_bytes_per_sec_rate, read_iops_rate, "
           "write_iops_rate, "
        << "avg_read_latency_ms_rate, avg_write_latency_ms_rate, "
           "util_percent_rate, "
        << "timestamp) VALUES ('" << host_name << "','" << disk_name << "',"
        << disk.reads() << "," << disk.writes() << "," << disk.sectors_read()
        << "," << disk.sectors_written() << "," << disk.read_time_ms() << ","
        << disk.write_time_ms() << "," << disk.io_in_progress() << ","
        << disk.io_time_ms() << "," << disk.weighted_io_time_ms() << ","
        << curr.read_bytes_per_sec << "," << curr.write_bytes_per_sec << ","
        << curr.read_iops << "," << curr.write_iops << ","
        << curr.avg_read_latency_ms << "," << curr.avg_write_latency_ms << ","
        << curr.util_percent << ","
        << rate(curr.read_bytes_per_sec, last.read_bytes_per_sec) << ","
        << rate(curr.write_bytes_per_sec, last.write_bytes_per_sec) << ","
        << rate(curr.read_iops, last.read_iops) << ","
        << rate(curr.write_iops, last.write_iops) << ","
        << rate(curr.avg_read_latency_ms, last.avg_read_latency_ms) << ","
        << rate(curr.avg_write_latency_ms, last.avg_write_latency_ms) << ","
        << rate(curr.util_percent, last.util_percent) << ",'" << time_buf
        << "')";
    mysql_query(conn, oss.str().c_str());

    last = curr;
  }

#else
  (void)host_name;
  (void)host_score;
  (void)net_in_rate;
  (void)net_out_rate;
  (void)cpu_percent_rate;
  (void)usr_percent_rate;
  (void)system_percent_rate;
  (void)nice_percent_rate;
  (void)idle_percent_rate;
  (void)io_wait_percent_rate;
  (void)irq_percent_rate;
  (void)soft_irq_percent_rate;
  (void)steal_percent_rate;
  (void)guest_percent_rate;
  (void)guest_nice_percent_rate;
  (void)load_avg_1_rate;
  (void)load_avg_3_rate;
  (void)load_avg_15_rate;
  (void)mem_used_percent_rate;
  (void)mem_total_rate;
  (void)mem_free_rate;
  (void)mem_avail_rate;
  (void)net_in_rate_rate;
  (void)net_out_rate_rate;
  (void)net_in_drop_rate_rate;
  (void)net_out_drop_rate_rate;
#endif
}

}  // namespace monitor
