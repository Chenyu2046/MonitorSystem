/**
 * @file host_manager.cpp
 * @brief Manager 主机消息处理、评分、诊断证据和持久化任务编排实现。
 *
 * 处理链路：gRPC MonitorInfo -> Submit -> host shard -> ProcessOne ->
 * CPU Overview/变化率/Score -> EvidenceBuilder -> RootCauseEngine /
 * IncidentStore -> PersistenceTask -> MySQL worker。普通 host_scores_ 和
 * 诊断 IncidentStore 是内存快路径，MySQL 不可用时不阻断消息处理。
 */

#include "host_manager.h"

#include "canonical_host_key.h"
#include "perf/perf_log.h"

#include "mysql_timeout_config.h"
#include "mysql_schema.h"
#include "health/top_signal_codec.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string_view>
#include <thread>
#include <utility>

#ifdef ENABLE_MYSQL
#include <mysql/mysql.h>
#endif

namespace monitor {

std::optional<std::size_t> ParsePositiveSizeConfig(
    const char* value, std::size_t default_value) {
  if (!value) return default_value;
  const std::string_view text(value);
  std::size_t parsed = 0;
  const auto result =
      std::from_chars(text.data(), text.data() + text.size(), parsed);
  if (result.ec != std::errc() || result.ptr != text.data() + text.size() ||
      parsed == 0) {
    return std::nullopt;
  }
  return parsed;
}

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

std::size_t DefaultShardCount() {
  const auto hardware = std::thread::hardware_concurrency();
  const auto half = hardware == 0 ? 2u : std::max(2u, hardware / 2);
  return std::min<std::size_t>(half, 8);
}

bool IsValidMonitorInfo(const monitor::proto::MonitorInfo& info) {
  const auto cpu = BuildCpuOverview(info);
  if (CanonicalHostKey(info).empty() || cpu.cpu_count == 0) return false;
  for (const auto& core : info.cpu_stat()) {
    if (!core.sample_valid() || !std::isfinite(core.cpu_percent()) ||
        !std::isfinite(core.usr_percent()) ||
        !std::isfinite(core.system_percent()) ||
        !std::isfinite(core.nice_percent()) ||
        !std::isfinite(core.idle_percent()) ||
        !std::isfinite(core.io_wait_percent()) ||
        !std::isfinite(core.irq_percent()) ||
        !std::isfinite(core.soft_irq_percent()) || core.cpu_percent() < 0 ||
        core.cpu_percent() > 100 || core.usr_percent() < 0 ||
        core.usr_percent() > 100 || core.system_percent() < 0 ||
        core.system_percent() > 100 || core.nice_percent() < 0 ||
        core.nice_percent() > 100 || core.idle_percent() < 0 ||
        core.idle_percent() > 100 || core.io_wait_percent() < 0 ||
        core.io_wait_percent() > 100 || core.irq_percent() < 0 ||
        core.irq_percent() > 100 || core.soft_irq_percent() < 0 ||
        core.soft_irq_percent() > 100) {
      return false;
    }
  }
  for (const auto& value : {cpu.cpu_percent, cpu.usr_percent,
                            cpu.system_percent, cpu.nice_percent,
                            cpu.idle_percent, cpu.io_wait_percent,
                            cpu.irq_percent, cpu.soft_irq_percent}) {
    if (!std::isfinite(value) || value < 0 || value > 100) return false;
  }
  if (!info.has_cpu_load() || !info.cpu_load().sample_valid() ||
      !std::isfinite(info.cpu_load().load_avg_1()) ||
      !std::isfinite(info.cpu_load().load_avg_3()) ||
      !std::isfinite(info.cpu_load().load_avg_15()) ||
      info.cpu_load().load_avg_1() < 0 || info.cpu_load().load_avg_3() < 0 ||
      info.cpu_load().load_avg_15() < 0 || !info.has_mem_info() ||
      !info.mem_info().sample_valid() ||
      !std::isfinite(info.mem_info().used_percent()) ||
      info.mem_info().used_percent() < 0 ||
      info.mem_info().used_percent() > 100 ||
      !std::isfinite(info.mem_info().total()) || info.mem_info().total() <= 0 ||
      !std::isfinite(info.mem_info().free()) ||
      info.mem_info().free() < 0 || !std::isfinite(info.mem_info().avail()) ||
      info.mem_info().avail() < 0) {
    return false;
  }
  const auto network = BuildNetworkOverview(info);
  if (network.interface_count == 0 || info.disk_info_size() == 0) return false;
  for (const auto& net : info.net_info()) {
    if (!net.sample_valid() || !std::isfinite(net.rcv_rate()) ||
        !std::isfinite(net.send_rate()) ||
        !std::isfinite(net.rcv_packets_rate()) ||
        !std::isfinite(net.send_packets_rate()) || net.rcv_rate() < 0 ||
        net.send_rate() < 0 || net.rcv_packets_rate() < 0 ||
        net.send_packets_rate() < 0) {
      return false;
    }
  }
  for (const auto& disk : info.disk_info()) {
    if (!disk.sample_valid() || !std::isfinite(disk.util_percent()) ||
        disk.util_percent() < 0 ||
        disk.util_percent() > 100 ||
        !std::isfinite(disk.read_bytes_per_sec()) ||
        !std::isfinite(disk.write_bytes_per_sec()) ||
        !std::isfinite(disk.read_iops()) || !std::isfinite(disk.write_iops()) ||
        !std::isfinite(disk.avg_read_latency_ms()) ||
        !std::isfinite(disk.avg_write_latency_ms()) ||
        disk.read_bytes_per_sec() < 0 || disk.write_bytes_per_sec() < 0 ||
        disk.read_iops() < 0 || disk.write_iops() < 0 ||
        disk.avg_read_latency_ms() < 0 || disk.avg_write_latency_ms() < 0) {
      return false;
    }
  }
  return true;
}

}  // namespace

/**
 * @brief 计算普通主机 CPU 概览：有效核平均值 + 最忙核。
 *
 * NaN/Inf 样本不参与平均值和计数；平均值用于主机概览，peak core 只用于
 * 辅助观察，异常诊断路径仍在 Worker/证据构建阶段保留逐核最大值语义。
 */
CpuOverview BuildCpuOverview(const monitor::proto::MonitorInfo& info) {
  CpuOverview overview;
  for (const auto& cpu : info.cpu_stat()) {
    const bool valid = std::isfinite(cpu.cpu_percent()) &&
                       std::isfinite(cpu.usr_percent()) &&
                       std::isfinite(cpu.system_percent()) &&
                       std::isfinite(cpu.nice_percent()) &&
                       std::isfinite(cpu.idle_percent()) &&
                       std::isfinite(cpu.io_wait_percent()) &&
                       std::isfinite(cpu.irq_percent()) &&
                       std::isfinite(cpu.soft_irq_percent());
    if (!valid) {
      continue;
    }

    // 这里只聚合有效核；一个异常非有限值不能污染整机平均值。
    overview.cpu_percent += cpu.cpu_percent();
    overview.usr_percent += cpu.usr_percent();
    overview.system_percent += cpu.system_percent();
    overview.nice_percent += cpu.nice_percent();
    overview.idle_percent += cpu.idle_percent();
    overview.io_wait_percent += cpu.io_wait_percent();
    overview.irq_percent += cpu.irq_percent();
    overview.soft_irq_percent += cpu.soft_irq_percent();

    if (overview.cpu_count == 0 ||
        cpu.cpu_percent() > overview.peak_cpu_percent) {
      overview.peak_core_name = cpu.cpu_name();
      overview.peak_cpu_percent = cpu.cpu_percent();
    }
    ++overview.cpu_count;
  }

  if (overview.cpu_count == 0) {
    return overview;
  }

  // 普通主机概览使用所有有效核的平均值；例如一个 95% 热点核可能被
  // 其他空闲核摊平，但异常检测仍需使用 max-core 逻辑。
  const float divisor = static_cast<float>(overview.cpu_count);
  overview.cpu_percent /= divisor;
  overview.usr_percent /= divisor;
  overview.system_percent /= divisor;
  overview.nice_percent /= divisor;
  overview.idle_percent /= divisor;
  overview.io_wait_percent /= divisor;
  overview.irq_percent /= divisor;
  overview.soft_irq_percent /= divisor;
  return overview;
}

NetworkOverview BuildNetworkOverview(const monitor::proto::MonitorInfo& info) {
  NetworkOverview overview;
  for (const auto& net : info.net_info()) {
    if (!std::isfinite(net.rcv_rate()) || !std::isfinite(net.send_rate()) ||
        !std::isfinite(net.rcv_packets_rate()) ||
        !std::isfinite(net.send_packets_rate()) || net.rcv_rate() < 0 ||
        net.send_rate() < 0 || net.rcv_packets_rate() < 0 ||
        net.send_packets_rate() < 0) {
      continue;
    }
    overview.reported_total_recv_kib_per_sec += net.rcv_rate();
    overview.reported_total_send_kib_per_sec += net.send_rate();
    overview.peak_recv_kib_per_sec =
        std::max(overview.peak_recv_kib_per_sec,
                 static_cast<double>(net.rcv_rate()));
    overview.peak_send_kib_per_sec =
        std::max(overview.peak_send_kib_per_sec,
                 static_cast<double>(net.send_rate()));
    overview.total_packets_per_sec += net.rcv_packets_rate();
    overview.total_packets_per_sec += net.send_packets_rate();
    ++overview.interface_count;
  }
  return overview;
}

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

HostManager::HostManager(std::chrono::milliseconds health_maintenance_interval,
                         std::chrono::milliseconds health_max_idle)
    : running_(false),
      health_maintenance_interval_(health_maintenance_interval),
      health_max_idle_(health_max_idle) {}

HostManager::~HostManager() {
  Stop();
#ifdef ENABLE_MYSQL
  CloseMysqlConnection();
#endif
}

bool HostManager::Start() {
  // Start 只允许一次：先初始化诊断持久化能力，再启动持久化 worker、
  // host shard workers 和 stale-host 管理线程，确保提交路径有消费者。
  bool expected = false;
  if (!running_.compare_exchange_strong(expected, true)) {
    return false;
  }
  if (health_maintenance_interval_.count() <= 0 ||
      health_max_idle_.count() <= 0) {
    std::cerr << "invalid health maintenance interval\n";
    running_.store(false);
    return false;
  }

  const auto resolve_size = [](const char* name, std::size_t default_value) {
    const auto parsed =
        ParsePositiveSizeConfig(std::getenv(name), default_value);
    if (!parsed) std::cerr << "invalid Manager config: " << name << "\n";
    return parsed;
  };
  const auto shard_count =
      resolve_size("KERNSCOPE_MANAGER_SHARDS", DefaultShardCount());
  const auto shard_queue_capacity =
      resolve_size("KERNSCOPE_SHARD_QUEUE_CAPACITY", 256);
  const auto shard_queue_max_bytes = resolve_size(
      "KERNSCOPE_SHARD_QUEUE_MAX_BYTES", 64ull * 1024ull * 1024ull);
  const auto persistence_queue_capacity =
      resolve_size("KERNSCOPE_PERSIST_QUEUE_CAPACITY", 1024);
  const auto persistence_queue_max_bytes = resolve_size(
      "KERNSCOPE_PERSIST_QUEUE_MAX_BYTES", 128ull * 1024ull * 1024ull);
  if (!shard_count || !shard_queue_capacity || !shard_queue_max_bytes ||
      !persistence_queue_capacity || !persistence_queue_max_bytes) {
    running_.store(false);
    return false;
  }
  health::HealthConfig health_config;
  std::string health_error;
  if (!health::LoadHealthConfigFromEnvironment(&health_config, &health_error)) {
    std::cerr << health_error << "\n";
    running_.store(false);
    return false;
  }
  health_config_ = health_config;
#ifdef ENABLE_MYSQL
  if (!GetMysqlConnection()) {
    running_.store(false);
    return false;
  }
  if (!EnsureOrdinarySchemaReady(GetMysqlConnection())) {
    running_.store(false);
    return false;
  }
  const bool persistence_initialized = diagnostic_persistence_.Init(
      GetEnvOrDefault("MONITOR_MYSQL_HOST", MYSQL_HOST),
      GetEnvOrDefault("MONITOR_MYSQL_USER", MYSQL_USER),
      GetEnvOrDefault("MONITOR_MYSQL_PASSWORD", MYSQL_PASS),
      GetEnvOrDefault("MONITOR_MYSQL_DATABASE", MYSQL_DB));
  diagnostic_persistence_state_.SetInitialized(persistence_initialized);
  if (!persistence_initialized) {
    running_.store(false);
    return false;
  }
#else
  diagnostic_persistence_state_.SetInitialized(false);
#endif

  shard_perf_samples_.clear();
  shard_perf_samples_.resize(*shard_count);
  shard_health_engines_.clear();
  shard_health_engines_.resize(*shard_count);
  shard_feedback_caches_.clear();
  shard_feedback_caches_.resize(*shard_count);

  persistence_worker_ = std::make_unique<PersistenceWorker>(
      *persistence_queue_capacity, *persistence_queue_max_bytes,
      [this](PersistenceTask&& task) {
        const bool success = PersistTask(std::move(task));
        if (!success) {
          persistence_failed_count_.fetch_add(1, std::memory_order_relaxed);
        }
        return success;
      });
  persistence_worker_->Start();

  shard_executor_ = std::make_unique<HostShardExecutor>(
      *shard_count, *shard_queue_capacity, *shard_queue_max_bytes,
      [this](std::size_t shard_id, const std::string& host_name,
             const monitor::proto::MonitorInfo& info,
             std::chrono::system_clock::time_point received_at,
             std::chrono::steady_clock::time_point enqueued_at) {
        return ProcessOne(shard_id, host_name, info, received_at, enqueued_at);
      },
      [this](std::size_t shard_id,
             std::chrono::steady_clock::time_point now) {
        MaintainShard(shard_id, now);
      },
      health_maintenance_interval_);
  shard_executor_->Start();
  thread_ = std::make_unique<std::thread>(&HostManager::ProcessLoop, this);
  return true;
}

void HostManager::Stop() {
  // 先停止新消息进入，再按 host shard -> persistence worker 顺序 join，
  // 让已接受的普通处理和持久化任务有明确的关闭边界。
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
            << " persistence_failed="
            << persistence_failed_count_.load(std::memory_order_relaxed)
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
  // 该线程不处理 MonitorInfo，只周期清理超过 60 秒未更新的 host_scores_；
  // mtx_ 同时保护 Submit/查询路径访问的主机评分表。
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
  // gRPC handler 到这里仍在接收线程；只做主机名校验和有界入队，避免
  // 在 RPC 线程执行 CPU 评分、规则诊断或 MySQL。
  const std::string host_name = CanonicalHostKey(info);
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

DataReceiveResult HostManager::SubmitWithFeedback(
    const monitor::proto::MonitorInfo& info,
    std::chrono::system_clock::time_point deadline,
    monitor::proto::MonitorFeedback* feedback) {
  if (feedback) feedback->Clear();
  const std::string host_name = CanonicalHostKey(info);
  if (host_name.empty()) return DataReceiveResult::kInvalidHost;
  if (!shard_executor_) return DataReceiveResult::kStopping;

  // Requests from old Workers have no correlation identity. They retain the
  // original asynchronous accepted semantics and receive no remote signal.
  if (!feedback || info.sample_sequence() == 0 ||
      info.sample_timestamp_ms() <= 0) {
    return Submit(info);
  }

  auto completion =
      std::make_shared<std::promise<HostFeedbackResult>>();
  auto completed = completion->get_future();
  const auto result = shard_executor_->SubmitTracked(host_name, info,
                                                      std::move(completion));
  if (result == DataReceiveResult::kAccepted) {
    accepted_count_.fetch_add(1, std::memory_order_relaxed);
  } else if (result == DataReceiveResult::kQueueFull) {
    queue_full_count_.fetch_add(1, std::memory_order_relaxed);
  }
  if (result != DataReceiveResult::kAccepted ||
      completed.wait_until(deadline) != std::future_status::ready) {
    return result;
  }

  const HostFeedbackResult work_result = completed.get();
  if (!work_result.health_valid || work_result.host_name != host_name ||
      work_result.result_timestamp_ms != info.sample_timestamp_ms() ||
      work_result.result_version != info.sample_sequence() ||
      !std::isfinite(work_result.node_anomaly_score)) {
    return result;
  }
  feedback->set_host_name(work_result.host_name);
  feedback->set_health_valid(true);
  feedback->set_node_anomaly_score(
      std::clamp(work_result.node_anomaly_score, 0.0, 1.0));
  feedback->set_remote_trigger_score(
      std::clamp(work_result.remote_trigger_score, 0.0, 1.0));
  feedback->set_result_timestamp_ms(work_result.result_timestamp_ms);
  feedback->set_result_version(work_result.result_version);
  return result;
}

void HostManager::MaintainShard(
    std::size_t shard_id, std::chrono::steady_clock::time_point now) {
  // The timed callback runs on this shard's sole worker, preserving ownership
  // of both maps even when no host submits another message.
  for (const auto& stale_host : health::PruneStaleHealthEngines(
           &shard_health_engines_[shard_id], now, health_max_idle_)) {
    shard_perf_samples_[shard_id].erase(stale_host);
    shard_feedback_caches_[shard_id].erase(stale_host);
    health_state_evicted_count_.fetch_add(1, std::memory_order_relaxed);
  }
}

// 计算主机的综合评分
HostFeedbackResult HostManager::ProcessOne(
    std::size_t shard_id, const std::string& host_name,
    const monitor::proto::MonitorInfo& info,
    std::chrono::system_clock::time_point received_at,
    std::chrono::steady_clock::time_point enqueued_at) {
  const auto process_start = std::chrono::steady_clock::now();
  const auto queue_wait_us = perf::ElapsedUs(enqueued_at, process_start);
  const auto validate_start = std::chrono::steady_clock::now();
  const bool monitor_valid = IsValidMonitorInfo(info);
  const auto validate_us = perf::ElapsedUs(validate_start);
  constexpr std::size_t kFeedbackCacheEntriesPerHost = 16;
  const bool has_sample_identity = info.sample_sequence() != 0;
  HostFeedbackCache* feedback_cache = nullptr;
  const auto log_session_reject = [&](const char* reason,
                                      const HostFeedbackCache& cache) {
    if (!perf::OutputEnabled()) return;
    perf::LogPerf("manager", "session_reject",
                  perf::BuildTraceId(host_name, info), [&] {
                    std::ostringstream output;
                    output << "reason=" << reason
                           << " latest_session=" << cache.latest_session_id
                           << " latest_sequence=" << cache.latest_sequence
                           << " incoming_session=" << info.sample_session_id()
                           << " incoming_sequence=" << info.sample_sequence();
                    return output.str();
                  });
  };
  if (has_sample_identity) {
    auto& feedback_map = shard_feedback_caches_[shard_id];
    const auto cache_it = feedback_map.find(host_name);
    if (cache_it != feedback_map.end()) {
      auto& cache = cache_it->second;
      for (const auto& entry : cache.entries) {
        if (entry.sequence == info.sample_sequence() &&
            entry.session_id == info.sample_session_id() &&
            (info.sample_session_id().empty()
                 ? entry.timestamp_ms == info.sample_timestamp_ms()
                 : true)) {
          log_session_reject("duplicate", cache);
          return entry.result;
        }
      }
      if (!info.sample_session_id().empty()) {
        if (std::find(cache.retired_session_ids.begin(),
                      cache.retired_session_ids.end(),
                      info.sample_session_id()) !=
            cache.retired_session_ids.end()) {
          log_session_reject("retired_session", cache);
          return {};
        }
        if (cache.latest_session_id == info.sample_session_id() &&
            info.sample_sequence() <= cache.latest_sequence) {
          log_session_reject("stale_sequence", cache);
          return {};
        }
      } else if (!cache.latest_session_id.empty()) {
        log_session_reject("legacy_after_modern", cache);
        return {};
      } else if (info.sample_timestamp_ms() < cache.latest_timestamp_ms ||
                 (info.sample_timestamp_ms() == cache.latest_timestamp_ms &&
                  info.sample_sequence() <= cache.latest_sequence)) {
        log_session_reject("legacy_timestamp_stale", cache);
        return {};
      }
      feedback_cache = &cache;
    }
  }

  const auto cache_result = [&](HostFeedbackResult result) {
    if (!feedback_cache) return result;
    if (!info.sample_session_id().empty() &&
        !feedback_cache->latest_session_id.empty() &&
        feedback_cache->latest_session_id != info.sample_session_id()) {
      feedback_cache->retired_session_ids.push_back(
          feedback_cache->latest_session_id);
      constexpr std::size_t kRetiredSessionsPerHost = 8;
      while (feedback_cache->retired_session_ids.size() >
             kRetiredSessionsPerHost) {
        feedback_cache->retired_session_ids.pop_front();
      }
    }
    feedback_cache->latest_session_id = info.sample_session_id();
    feedback_cache->latest_timestamp_ms = info.sample_timestamp_ms();
    feedback_cache->latest_sequence = info.sample_sequence();
    feedback_cache->entries.push_back(
        {info.sample_session_id(), info.sample_sequence(),
         info.sample_timestamp_ms(), result});
    while (feedback_cache->entries.size() >
           kFeedbackCacheEntriesPerHost) {
      feedback_cache->entries.pop_front();
    }
    return result;
  };

  const auto session_gate_us = perf::ElapsedUs(validate_start) - validate_us;
  // 一个 host 始终落到同一个 shard，因而 shard_perf_samples_[shard_id]
  // 的 previous/current 顺序成立，无需为每台主机增加独立锁。
  const auto resource_score_start = std::chrono::steady_clock::now();
  const ScoreResult score_result =
      monitor_valid ? CalcResourceScore(info) : ScoreResult{};
  const auto resource_score_us = perf::ElapsedUs(resource_score_start);
  const double score = score_result.score;
  health::HealthResult health_result;
  std::int64_t health_call_us = 0;
  if (monitor_valid && score_result.valid) {
    auto& health_engines = shard_health_engines_[shard_id];
    auto [engine_it, inserted] =
        health_engines.try_emplace(host_name, health_config_);
    (void)inserted;
    auto event_timestamp = received_at;
    if (info.sample_timestamp_ms() > 0) {
      event_timestamp = std::chrono::system_clock::time_point(
          std::chrono::milliseconds(info.sample_timestamp_ms()));
    }
    const auto health_start = std::chrono::steady_clock::now();
    health_result = engine_it->second.Evaluate(info, event_timestamp, score,
                                               enqueued_at);
    health_call_us = perf::ElapsedUs(health_start);
  }
  HostFeedbackResult feedback_result;
  if (has_sample_identity && score_result.valid && health_result.valid &&
      std::isfinite(health_result.anomaly_score)) {
    feedback_result.host_name = host_name;
    feedback_result.node_anomaly_score =
        std::clamp(health_result.anomaly_score, 0.0, 1.0);
    feedback_result.remote_trigger_score =
        std::clamp(health_result.remote_trigger_score, 0.0, 1.0);
    feedback_result.result_timestamp_ms = info.sample_timestamp_ms();
    feedback_result.result_version = info.sample_sequence();
    feedback_result.health_valid = true;
  }
  if (!score_result.valid || !monitor_valid) {
    if (perf::OutputEnabled()) {
      perf::LogPerf("manager", "monitor_invalid",
                    perf::BuildTraceId(host_name, info), [&] {
                      return "validate_us=" + std::to_string(validate_us) +
                             " session_gate_us=" +
                             std::to_string(session_gate_us);
                    });
    }
    std::lock_guard<std::mutex> lock(mtx_);
    host_scores_[host_name] = HostScore{info, score, false, received_at,
                                       health_result};
    return feedback_result;
  }
  if (has_sample_identity && !feedback_cache) {
    auto& feedback_map = shard_feedback_caches_[shard_id];
    auto [cache_it, inserted] = feedback_map.try_emplace(host_name);
    (void)inserted;
    feedback_cache = &cache_it->second;
  }
  const auto now = received_at;
  const auto queue_delay_us = static_cast<std::uint64_t>(
      std::max<std::int64_t>(0, queue_wait_us));
  queue_delay_samples_.fetch_add(1, std::memory_order_relaxed);
  queue_delay_total_us_.fetch_add(queue_delay_us, std::memory_order_relaxed);
  auto current_max = max_queue_delay_us_.load(std::memory_order_relaxed);
  while (current_max < queue_delay_us &&
         !max_queue_delay_us_.compare_exchange_weak(
             current_max, queue_delay_us, std::memory_order_relaxed,
             std::memory_order_relaxed)) {
  }

  const NetworkOverview network = BuildNetworkOverview(info);
  const double net_in_rate = network.peak_recv_kib_per_sec;
  const double net_out_rate = network.peak_send_kib_per_sec;

  // 当前采样：PerfSample 只保存下一轮变化率所需的基础值，不替代原始
  // MonitorInfo，也不改变 protobuf 的字段语义。
  const auto perf_rate_start = std::chrono::steady_clock::now();
  PerfSample curr;
  const CpuOverview cpu = BuildCpuOverview(info);
  curr.cpu_percent = cpu.cpu_percent;
  curr.usr_percent = cpu.usr_percent;
  curr.system_percent = cpu.system_percent;
  curr.nice_percent = cpu.nice_percent;
  curr.idle_percent = cpu.idle_percent;
  curr.io_wait_percent = cpu.io_wait_percent;
  curr.irq_percent = cpu.irq_percent;
  curr.soft_irq_percent = cpu.soft_irq_percent;
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

  // 变化率计算：同一 Host 始终由同一个 shard worker 访问该状态，避免
  // 多线程同时更新同一 host 的 previous snapshot。
  auto& last = shard_perf_samples_[shard_id][host_name];
  auto rate = [](float now_val, float last_val) -> float {
    if (last_val == 0) return 0;
    return (now_val - last_val) / last_val;
  };

  PersistenceTask task;
  task.host_name = host_name;
  task.host_score =
      HostScore{info, score, score_result.valid, now, health_result};
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
  const auto perf_rate_us = perf::ElapsedUs(perf_rate_start);

  const auto host_score_update_start = std::chrono::steady_clock::now();
  {
    std::lock_guard<std::mutex> lock(mtx_);
    host_scores_[host_name] = task.host_score;
  }
  const auto host_score_update_us =
      perf::ElapsedUs(host_score_update_start);

  std::int64_t evidence_us = 0;
  std::int64_t rca_us = 0;
  std::int64_t incident_us = 0;
  std::size_t evidence_count = 0;
  std::size_t root_cause_count = 0;
  bool incident_created = false;
  std::uint64_t incident_id = 0;

  if (info.has_diagnostic()) {
    // Health signals enrich Evidence/RCA only. Worker state remains authoritative;
    // there is deliberately no Manager -> Worker probe-control RPC.
    const auto evidence_start = std::chrono::steady_clock::now();
    const auto evidence =
        evidence_builder_.Build(info, now, &health_result);
    evidence_us = perf::ElapsedUs(evidence_start);
    evidence_count = evidence.size();
    const auto rca_start = std::chrono::steady_clock::now();
    const auto root_causes = root_cause_engine_.Evaluate(evidence);
    rca_us = perf::ElapsedUs(rca_start);
    root_cause_count = root_causes.size();
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
    const auto incident_start = std::chrono::steady_clock::now();
    task.incident = incident_store_.Observe(
        host_name, DiagnosticStateName(info.diagnostic().state()), evidence,
        root_causes, now);
    incident_us = perf::ElapsedUs(incident_start);
    incident_created = task.incident.has_value();
    if (task.incident) incident_id = task.incident->id;
  }

  if (IsMetricsLogEnabled()) {
    std::cout << "\n================== Received Data =================="
              << std::endl;
    std::cout << "Server: " << host_name << ", Score: " << score << std::endl;

    // CPU 详细信息
    std::cout << "\n--- CPU ---" << std::endl;
    std::cout << "  Host CPU: " << curr.cpu_percent << "%, "
              << "User: " << curr.usr_percent << "%, "
              << "System: " << curr.system_percent << "%" << std::endl;
    std::cout << "  Nice: " << curr.nice_percent << "%, "
              << "Idle: " << curr.idle_percent << "%, "
              << "IOWait: " << curr.io_wait_percent << "%" << std::endl;
    std::cout << "  IRQ: " << curr.irq_percent << "%, "
              << "SoftIRQ: " << curr.soft_irq_percent << "%" << std::endl;
    if (cpu.cpu_count > 0) {
      std::cout << "  Peak Core: " << cpu.peak_core_name << " at "
                << cpu.peak_cpu_percent << "%" << std::endl;
    }
    std::cout << "  Load: " << curr.load_avg_1 << "/" << curr.load_avg_3 << "/"
              << curr.load_avg_15 << std::endl;

    // 内存详细信息
    std::cout << "\n--- Memory ---" << std::endl;
    std::cout << "  Used: " << curr.mem_used_percent << "%, "
              << "Total: " << curr.mem_total << " GiB" << std::endl;
    std::cout << "  Free: " << curr.mem_free << " GiB, "
              << "Avail: " << curr.mem_avail << " GiB" << std::endl;

    // 网络详细信息
    std::cout << "\n--- Network ---" << std::endl;
    std::cout << "  In: " << net_in_rate << " KiB/s, "
              << "Out: " << net_out_rate << " KiB/s" << std::endl;
    for (int i = 0; i < info.net_info_size(); ++i) {
      const auto& net = info.net_info(i);
      std::cout << "  [" << net.name() << "] Recv: " << net.rcv_rate()
                << " KiB/s, "
                << "Send: " << net.send_rate() << " KiB/s, "
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
  const bool has_incident = task.incident.has_value();
  const auto persistence_enqueue_start = std::chrono::steady_clock::now();
  const bool persistence_enqueued =
      persistence_worker_ && persistence_worker_->Enqueue(std::move(task));
  const auto persistence_enqueue_us =
      perf::ElapsedUs(persistence_enqueue_start);
  if (persistence_enqueued) {
    processed_count_.fetch_add(1, std::memory_order_relaxed);
  } else {
    persistence_rejected_count_.fetch_add(1, std::memory_order_relaxed);
    std::cerr << "ERROR: persistence task rejected (worker stopped, queue "
                 "closed, or task exceeded the byte budget)"
              << std::endl;
  }
  const auto process_total_us = perf::ElapsedUs(process_start);
  const bool slow_queue = perf::IsSlow(
      static_cast<std::int64_t>(queue_wait_us),
      perf::GetConfig().slow_manager_queue_ms);
  const bool slow_process = perf::IsSlow(
      process_total_us, perf::GetConfig().slow_manager_process_ms);
  if (perf::PerfTraceEnabled() || slow_queue || slow_process) {
    const auto trace_id = perf::BuildTraceId(host_name, info);
    const auto fields = [&] {
      std::ostringstream output;
      output << "host=" << host_name << " shard=" << shard_id
             << " seq=" << info.sample_sequence()
             << " queue_wait_us=" << queue_wait_us
             << " validate_us=" << validate_us
             << " session_gate_us=" << session_gate_us
             << " resource_score_us=" << resource_score_us
             << " health_call_us=" << health_call_us
             << " perf_rate_us=" << perf_rate_us
             << " host_score_update_us=" << host_score_update_us
             << " evidence_us=" << evidence_us << " rca_us=" << rca_us
             << " incident_us=" << incident_us
             << " persistence_enqueue_us=" << persistence_enqueue_us
             << " evidence_count=" << evidence_count
             << " root_cause_count=" << root_cause_count
             << " incident_created=" << (incident_created ? 1 : 0)
             << " incident_id=" << incident_id
             << " process_total_us=" << process_total_us
             << " health_valid=" << (health_result.valid ? 1 : 0)
             << " persistence_enqueued=" << (persistence_enqueued ? 1 : 0)
             << " has_incident=" << (has_incident ? 1 : 0)
             << " result=ok";
      if (slow_queue || slow_process) {
        output << " reason=";
        bool first_reason = true;
        if (slow_queue) {
          output << "queue_wait";
          first_reason = false;
        }
        if (slow_process) {
          if (!first_reason) output << ",";
          output << "process";
        }
      }
      return output.str();
    };
    if (perf::PerfTraceEnabled()) {
      perf::LogPerf("manager", "sample_process", trace_id, fields);
    } else {
      perf::LogSlow("manager", "sample_process", trace_id, fields);
    }
  }
  return cache_result(std::move(feedback_result));
}

bool HostManager::PersistTask(PersistenceTask task) {
  // PersistenceWorker 串行调用此回调；这里执行普通历史写入和可选的
  // incident/evidence/root-cause 持久化，接收和 shard 处理线程不被 SQL 阻塞。
  // Only the single PersistenceWorker thread calls this method. The legacy
  // WriteToMysql detail-rate history therefore has one owner and no mutex.
  const auto persist_start = std::chrono::steady_clock::now();
  persistence_task_count_.fetch_add(1, std::memory_order_relaxed);
  bool success = true;
  std::int64_t diagnostic_persist_us = 0;
  if (task.incident) {
    const auto diagnostic_start = std::chrono::steady_clock::now();
    const bool persisted = diagnostic_persistence_.Save(*task.incident);
    diagnostic_persist_us = perf::ElapsedUs(diagnostic_start);
    success = persisted && success;
    diagnostic_persistence_state_.RecordSave(task.incident->id, persisted);
    if (!persisted) {
      std::cerr << "ERROR: diagnostic persistence degraded; incident "
                << task.incident->id << " remains available in memory"
                << std::endl;
    }
  }

  const auto mysql_start = std::chrono::steady_clock::now();
  success = WriteToMysql(task.host_name, task.host_score, task.net_in_rate,
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
               task.net_out_drop_rate_rate) && success;
  const auto mysql_us = perf::ElapsedUs(mysql_start);
  const auto persist_total_us = perf::ElapsedUs(persist_start);
  const bool slow_mysql =
      perf::IsSlow(mysql_us, perf::GetConfig().slow_mysql_ms);
  if (perf::PerfTraceEnabled() || slow_mysql) {
    const auto trace_id = perf::BuildTraceId(task.host_score.info);
    const auto fields = [&] {
      return "diagnostic_persist_us=" +
             std::to_string(diagnostic_persist_us) +
             " mysql_us=" + std::to_string(mysql_us) +
             " persist_total_us=" + std::to_string(persist_total_us) +
             " success=" + (success ? std::string("1") : std::string("0"));
    };
    if (perf::PerfTraceEnabled()) {
      perf::LogPerf("manager", "persistence", trace_id, fields);
    } else {
      perf::LogSlow("manager", "persistence", trace_id, fields);
    }
  }
  return success;
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
    if (!data.score_valid) continue;
    if (data.score > best_score) {
      best_score = data.score;
      best_host = host;
    }
  }
  return best_host;
}

std::string HostManager::GetHealthiestHost() {
  std::lock_guard<std::mutex> lock(mtx_);
  std::string healthiest_host;
  double healthiest_score = -1;
  for (const auto& [host, data] : host_scores_) {
    if (!data.score_valid || !data.health.valid) continue;
    if (data.health.health_score > healthiest_score) {
      healthiest_score = data.health.health_score;
      healthiest_host = host;
    }
  }
  return healthiest_host;
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

ScoreResult HostManager::CalcResourceScore(
    const monitor::proto::MonitorInfo& info) {
  ScoreResult result;
  if (!IsValidMonitorInfo(info)) return result;
  const double cpu_weight = 0.35;
  const double mem_weight = 0.30;
  const double load_weight = 0.15;
  const double disk_weight = 0.15;
  const double net_weight = 0.05;

  const double load_coefficient = 1.5;       // I/O 密集型场景系数
  const double max_bandwidth = 125000000.0 / 1024.0;

  double cpu_percent = 0, load_avg_1 = 0, mem_percent = 0;
  double net_recv_rate = 0, net_send_rate = 0, disk_util = 0;
  int cpu_cores = 1;

  const CpuOverview cpu = BuildCpuOverview(info);
  if (cpu.cpu_count > 0) {
    cpu_percent = cpu.cpu_percent;
    cpu_cores = static_cast<int>(cpu.cpu_count);
    if (cpu_cores < 1) cpu_cores = 1;
  }
  if (info.has_cpu_load()) {
    load_avg_1 = info.cpu_load().load_avg_1();
  }
  if (info.has_mem_info()) {
    mem_percent = info.mem_info().used_percent();
  }
  const auto network = BuildNetworkOverview(info);
  net_recv_rate = network.peak_recv_kib_per_sec;
  net_send_rate = network.peak_send_kib_per_sec;
  if (info.disk_info_size() > 0) {
    for (int i = 0; i < info.disk_info_size(); ++i) {
      double util = info.disk_info(i).util_percent();
      if (util > disk_util) disk_util = util;
    }
  }

  const auto nonnegative = [](double value) { return std::max(0.0, value); };
  double cpu_score = nonnegative(1.0 - cpu_percent / 100.0);
  double mem_score = nonnegative(1.0 - mem_percent / 100.0);
  double load_score =
      nonnegative(1.0 - load_avg_1 / (cpu_cores * load_coefficient));
  double disk_score = nonnegative(1.0 - disk_util / 100.0);
  double net_recv_score = nonnegative(1.0 - net_recv_rate / max_bandwidth);
  double net_send_score = nonnegative(1.0 - net_send_rate / max_bandwidth);
  double net_score = (net_recv_score + net_send_score) / 2.0;

  result.score = (cpu_score * cpu_weight + mem_score * mem_weight +
                 load_score * load_weight + disk_score * disk_weight +
                 net_score * net_weight) *
                100.0;
  result.valid = std::isfinite(result.score);
  return result;
}

bool HostManager::WriteToMysql(
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
  // Legacy 普通监控写库路径保留现有表/字段语义；本任务只解释调用边界，
  // 不重排 SQL、不改变事务和连接策略。
#ifdef ENABLE_MYSQL
  struct MysqlPerfTrace {
    const monitor::proto::MonitorInfo& info;
    const std::chrono::steady_clock::time_point started =
        std::chrono::steady_clock::now();
    std::string trace_id;
    std::int64_t connect_us = 0;
    std::int64_t begin_us = 0;
    std::int64_t main_us = 0;
    std::int64_t net_us = 0;
    std::int64_t softirq_us = 0;
    std::int64_t mem_us = 0;
    std::int64_t disk_us = 0;
    std::int64_t commit_us = 0;
    std::int64_t rollback_us = 0;
    bool success = false;
    MYSQL* conn = nullptr;
    std::string failed_stage;

    ~MysqlPerfTrace() {
      const auto total_us = perf::ElapsedUs(started);
      const bool slow = perf::IsSlow(total_us, perf::GetConfig().slow_mysql_ms);
      const bool trace = perf::PerfTraceEnabled();
      if (!trace && !slow && failed_stage.empty()) return;
      if (trace_id.empty()) trace_id = perf::BuildTraceId(info);
      const auto fields = [&] {
        std::ostringstream output;
        output << "mysql_connect_us=" << connect_us
               << " mysql_begin_us=" << begin_us
               << " mysql_main_us=" << main_us
               << " mysql_net_us=" << net_us
               << " mysql_softirq_us=" << softirq_us
               << " mysql_mem_us=" << mem_us
               << " mysql_disk_us=" << disk_us
               << " mysql_commit_us=" << commit_us
               << " mysql_rollback_us=" << rollback_us
               << " mysql_total_us=" << total_us
               << " result=" << (success ? "success" : "failure");
        if (!failed_stage.empty()) output << " failed_stage=" << failed_stage;
        return output.str();
      };
      if (trace) {
        perf::LogPerf("manager", "mysql", trace_id, fields);
      } else if (slow) {
        perf::LogSlow("manager", "mysql", trace_id, fields);
      }
      if (!failed_stage.empty()) {
        perf::LogError("manager", "mysql_failure", trace_id, [&] {
          std::ostringstream output;
          output << "stage=" << failed_stage;
          if (conn) {
            output << " mysql_errno=" << mysql_errno(conn)
                   << " mysql_error=" << mysql_error(conn);
          }
          output << " mysql_rollback_us=" << rollback_us;
          return output.str();
        });
      }
    }
  } mysql_trace{host_score.info};
  if (perf::PerfTraceEnabled()) {
    mysql_trace.trace_id = perf::BuildTraceId(host_score.info);
  }
  const auto connect_start = std::chrono::steady_clock::now();
  MYSQL* conn = GetMysqlConnection();
  mysql_trace.conn = conn;
  mysql_trace.connect_us = perf::ElapsedUs(connect_start);
  if (!conn) {
    mysql_trace.failed_stage = "connect";
    return false;
  }
  const auto begin_start = std::chrono::steady_clock::now();
  const bool transaction_started = mysql_query(conn, "START TRANSACTION") == 0;
  mysql_trace.begin_us = perf::ElapsedUs(begin_start);
  if (!transaction_started) {
    mysql_trace.failed_stage = "begin";
    return false;
  }
  PersistenceHistory next_history = persistence_history_;
  const auto exec = [&](const std::string& sql, const char* stage) {
    if (mysql_query(conn, sql.c_str()) == 0) return true;
    mysql_trace.failed_stage = stage;
    const auto rollback_start = std::chrono::steady_clock::now();
    mysql_query(conn, "ROLLBACK");
    mysql_trace.rollback_us += perf::ElapsedUs(rollback_start);
    return false;
  };

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
  const auto main_start = std::chrono::steady_clock::now();
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
    send_rate = static_cast<float>(net_out_rate);
    rcv_rate = static_cast<float>(net_in_rate);
    const CpuOverview cpu = BuildCpuOverview(info);
    cpu_percent = cpu.cpu_percent;
    usr_percent = cpu.usr_percent;
    system_percent = cpu.system_percent;
    nice_percent = cpu.nice_percent;
    idle_percent = cpu.idle_percent;
    io_wait_percent = cpu.io_wait_percent;
    irq_percent = cpu.irq_percent;
    soft_irq_percent = cpu.soft_irq_percent;
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
    if (next_history.disk_util.count(host_name) &&
        next_history.disk_util[host_name] != 0) {
      disk_util_percent_rate =
          (disk_util_percent - next_history.disk_util[host_name]) /
          next_history.disk_util[host_name];
    }
    next_history.disk_util[host_name] = disk_util_percent;

    std::ostringstream oss;
    oss << "INSERT INTO server_performance "
        << "(server_name, cpu_percent, usr_percent, system_percent, "
           "nice_percent, "
        << "idle_percent, io_wait_percent, irq_percent, soft_irq_percent, "
        << "load_avg_1, load_avg_3, load_avg_15, "
        << "mem_used_percent, total, free, avail, "
        << "disk_util_percent, send_rate, rcv_rate, score, health_score, "
           "resource_score, anomaly_score, anomaly_rate_5m, confidence, "
           "health_state, health_model_state, health_valid, "
           "health_top_signals, "
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
        << "," << host_score.score << ",";
    if (host_score.health.valid) {
      oss << host_score.health.health_score << "," << host_score.score << ","
          << host_score.health.anomaly_score << ","
          << host_score.health.anomaly_rate_5m << ","
          << host_score.health.confidence << ",'" << host_score.health.state
          << "','" << health::ModelStateName(host_score.health.model_state)
          << "',1,'" << health::EncodeTopSignals(
                              host_score.health.top_signals)
          << "',";
    } else {
      oss << "NULL," << host_score.score
          << ",NULL,NULL,NULL,NULL,NULL,0,NULL,";
    }
    oss << cpu_percent_rate << ","
        << usr_percent_rate << "," << system_percent_rate << ","
        << nice_percent_rate << "," << idle_percent_rate << ","
        << io_wait_percent_rate << "," << irq_percent_rate << ","
        << soft_irq_percent_rate << "," << load_avg_1_rate << ","
        << load_avg_3_rate << "," << load_avg_15_rate << ","
        << mem_used_percent_rate << "," << mem_total_rate << ","
        << mem_free_rate << "," << mem_avail_rate << ","
        << disk_util_percent_rate << "," << net_out_rate_rate << ","
        << net_in_rate_rate << ",'" << time_buf << "')";
    if (!exec(oss.str(), "server_performance")) {
      mysql_trace.main_us = perf::ElapsedUs(main_start);
      return false;
    }
  }
  mysql_trace.main_us = perf::ElapsedUs(main_start);

  // ========== 2. 写入网络详细表 server_net_detail ==========
  const auto net_start = std::chrono::steady_clock::now();
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
        next_history.net_samples[host_name][net_name];

    // 计算错误/丢弃变化率
    auto rate_u64 = [](uint64_t now_val, uint64_t last_val) -> std::string {
      if (last_val == 0 || now_val < last_val) return "NULL";
      return std::to_string(static_cast<float>(now_val - last_val) /
                            static_cast<float>(last_val));
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
    if (!exec(oss.str(), "server_net_detail")) {
      mysql_trace.net_us = perf::ElapsedUs(net_start);
      return false;
    }

    last = curr;
  }
  mysql_trace.net_us = perf::ElapsedUs(net_start);

  // ========== 3. 写入软中断详细表 server_softirq_detail ==========
  const auto softirq_start = std::chrono::steady_clock::now();
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
        next_history.softirq_samples[host_name][cpu_name];

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
    if (!exec(oss.str(), "server_softirq_detail")) {
      mysql_trace.softirq_us = perf::ElapsedUs(softirq_start);
      return false;
    }

    last = curr;
  }
  mysql_trace.softirq_us = perf::ElapsedUs(softirq_start);

  // ========== 4. 写入内存详细表 server_mem_detail ==========
  const auto mem_start = std::chrono::steady_clock::now();
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

    MemDetailSample& last = next_history.mem_samples[host_name];

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
    if (!exec(oss.str(), "server_mem_detail")) {
      mysql_trace.mem_us = perf::ElapsedUs(mem_start);
      return false;
    }

    last = curr;
  }
  mysql_trace.mem_us = perf::ElapsedUs(mem_start);

  // ========== 5. 写入磁盘详细表 server_disk_detail ==========
  const auto disk_start = std::chrono::steady_clock::now();
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
        next_history.disk_samples[host_name][disk_name];

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
    if (!exec(oss.str(), "server_disk_detail")) {
      mysql_trace.disk_us = perf::ElapsedUs(disk_start);
      return false;
    }

    last = curr;
  }
  mysql_trace.disk_us = perf::ElapsedUs(disk_start);

  const auto commit_start = std::chrono::steady_clock::now();
  const bool committed = mysql_query(conn, "COMMIT") == 0;
  mysql_trace.commit_us = perf::ElapsedUs(commit_start);
  if (!committed) {
    mysql_trace.failed_stage = "commit";
    const auto rollback_start = std::chrono::steady_clock::now();
    mysql_query(conn, "ROLLBACK");
    mysql_trace.rollback_us += perf::ElapsedUs(rollback_start);
    return false;
  }
  persistence_history_ = std::move(next_history);
  mysql_trace.success = true;

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
  return true;
}

}  // namespace monitor
