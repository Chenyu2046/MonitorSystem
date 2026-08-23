#pragma once

/**
 * @file query_manager.h
 * @brief Manager 历史性能、趋势、异常、详细资源和 incident 查询接口。
 *
 * QueryManager 将分页/时间范围/阈值请求转换为 MySQL 查询并解析为 C++
 * 中间记录；QueryServiceImpl 再把这些记录转换为 response protobuf。普通
 * 性能表是主表，Net/Disk/Mem/SoftIRQ 是详细资源数据，incident 查询还
 * 可回读 evidence/root-cause 子表。
 */

#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "diagnostics/incident_store.h"

#ifdef ENABLE_MYSQL
#include <mysql/mysql.h>
#endif

namespace monitor {

/** @brief 评分排序方向，与 query_api.proto 保持对应。 */
enum class SortOrder { DESC = 0, ASC = 1 };

/** @brief 查询结果中的服务器在线状态。 */
enum class ServerStatus { ONLINE = 0, OFFLINE = 1 };

/** @brief QueryAnomaly 的 CPU/内存/磁盘/变化率阈值。 */
struct AnomalyThresholds {
  float cpu_threshold = 80.0f;
  float mem_threshold = 90.0f;
  float disk_threshold = 85.0f;
  float change_rate_threshold = 0.5f;  // 50%
};

/** @brief 查询使用的闭区间时间范围。 */
struct TimeRange {
  std::chrono::system_clock::time_point start_time;
  std::chrono::system_clock::time_point end_time;
};

/**
 * @brief 主表中的主机性能快照及变化率。
 *
 * 普通字段是本轮值，*_rate 是相对上一轮/聚合窗口的变化率，不表示
 * 绝对资源值；详细资源记录由下方专用结构承载。
 */
struct PerformanceRecord {
  std::string server_name;
  std::chrono::system_clock::time_point timestamp;
  // CPU指标
  float cpu_percent = 0;
  float usr_percent = 0;
  float system_percent = 0;
  float nice_percent = 0;
  float idle_percent = 0;
  float io_wait_percent = 0;
  float irq_percent = 0;
  float soft_irq_percent = 0;
  // 负载指标
  float load_avg_1 = 0;
  float load_avg_3 = 0;
  float load_avg_15 = 0;
  // 内存指标
  float mem_used_percent = 0;
  float mem_total = 0;
  float mem_free = 0;
  float mem_avail = 0;
  // 磁盘指标
  float disk_util_percent = 0;
  // 网络指标
  float send_rate = 0;
  float rcv_rate = 0;
  // 评分
  float score = 0;
  // 变化率
  float cpu_percent_rate = 0;
  float mem_used_percent_rate = 0;
  float disk_util_percent_rate = 0;
  float load_avg_1_rate = 0;
  float send_rate_rate = 0;
  float rcv_rate_rate = 0;
};

/** @brief 根据阈值查询得到的一条异常记录。 */
struct AnomalyRecord {
  std::string server_name;
  std::chrono::system_clock::time_point timestamp;
  std::string anomaly_type;  // CPU_HIGH, MEM_HIGH, DISK_HIGH, RATE_SPIKE
  std::string severity;      // WARNING, CRITICAL
  float value = 0;
  float threshold = 0;
  std::string metric_name;
};

/** @brief 服务器最新 score 和关键指标摘要。 */
struct ServerScoreSummary {
  std::string server_name;
  float score = 0;
  std::chrono::system_clock::time_point last_update;
  ServerStatus status = ServerStatus::ONLINE;
  float cpu_percent = 0;
  float mem_used_percent = 0;
  float disk_util_percent = 0;
  float load_avg_1 = 0;
};

/** @brief 最新评分查询同时返回的集群聚合统计。 */
struct ClusterStats {
  int total_servers = 0;
  int online_servers = 0;
  int offline_servers = 0;
  float avg_score = 0;
  float max_score = 0;
  float min_score = 0;
  std::string best_server;
  std::string worst_server;
};

/** @brief 单网卡错误/丢弃和速率详细记录。 */
struct NetDetailRecord {
  std::string server_name;
  std::string net_name;
  std::chrono::system_clock::time_point timestamp;
  uint64_t err_in = 0;
  uint64_t err_out = 0;
  uint64_t drop_in = 0;
  uint64_t drop_out = 0;
  float rcv_bytes_rate = 0;
  float snd_bytes_rate = 0;
  float rcv_packets_rate = 0;
  float snd_packets_rate = 0;
};

/** @brief 单磁盘吞吐、IOPS、延迟和 util 详细记录。 */
struct DiskDetailRecord {
  std::string server_name;
  std::string disk_name;
  std::chrono::system_clock::time_point timestamp;
  float read_bytes_per_sec = 0;
  float write_bytes_per_sec = 0;
  float read_iops = 0;
  float write_iops = 0;
  float avg_read_latency_ms = 0;
  float avg_write_latency_ms = 0;
  float util_percent = 0;
};

/** @brief 主机内存分类详细记录，单位沿用查询表语义。 */
struct MemDetailRecord {
  std::string server_name;
  std::chrono::system_clock::time_point timestamp;
  float total = 0;
  float free = 0;
  float avail = 0;
  float buffers = 0;
  float cached = 0;
  float active = 0;
  float inactive = 0;
  float dirty = 0;
};

/** @brief 单 CPU 核各类 SoftIRQ 计数详细记录。 */
struct SoftIrqDetailRecord {
  std::string server_name;
  std::string cpu_name;
  std::chrono::system_clock::time_point timestamp;
  float hi = 0;
  float timer = 0;
  float net_tx = 0;
  float net_rx = 0;
  float block = 0;
  float irq_poll = 0;
  float tasklet = 0;
  float sched = 0;
  float hrtimer = 0;
  float rcu = 0;
};

/**
 * @brief 封装 MySQL 连接、查询分页、时间转换和诊断详情读取。
 *
 * 每个查询在 mtx_ 内使用当前连接；MySQL 未启用/未初始化时返回空结果。
 * 该类只负责历史查询，不替代 HostManager 的内存实时查询 fallback。
 */
class QueryManager {
 public:
  QueryManager();
  ~QueryManager();

  /** @brief 初始化 MySQL 连接和字符集。 */
  bool Init(const std::string& host, const std::string& user,
            const std::string& password, const std::string& database);

  /** @brief 关闭连接并清除 initialized 状态。 */
  void Close();

  bool IsInitialized();

  /** @brief 检查 start_time 不晚于 end_time。 */
  bool ValidateTimeRange(const TimeRange& range) const;

  /** @brief 分页查询主表性能快照及变化率。 */
  std::vector<PerformanceRecord> QueryPerformance(
      const std::string& server_name, const TimeRange& time_range, int page,
      int page_size, int* total_count);

  /** @brief 查询趋势，可按 interval_seconds 做时间桶聚合。 */
  std::vector<PerformanceRecord> QueryTrend(const std::string& server_name,
                                            const TimeRange& time_range,
                                            int interval_seconds);

  /** @brief 按阈值查询异常记录并分页返回。 */
  std::vector<AnomalyRecord> QueryAnomaly(const std::string& server_name,
                                          const TimeRange& time_range,
                                          const AnomalyThresholds& thresholds,
                                          int page, int page_size,
                                          int* total_count);

  /** @brief 按 score 升/降序分页查询服务器摘要。 */
  std::vector<ServerScoreSummary> QueryScoreRank(SortOrder order, int page,
                                                 int page_size,
                                                 int* total_count);

  /** @brief 查询每台服务器最新评分并生成集群统计。 */
  std::vector<ServerScoreSummary> QueryLatestScore(ClusterStats* stats);

  /** @brief 分页查询网卡详细历史数据。 */
  std::vector<NetDetailRecord> QueryNetDetail(const std::string& server_name,
                                              const TimeRange& time_range,
                                              int page, int page_size,
                                              int* total_count);

  /** @brief 分页查询磁盘详细历史数据。 */
  std::vector<DiskDetailRecord> QueryDiskDetail(const std::string& server_name,
                                                const TimeRange& time_range,
                                                int page, int page_size,
                                                int* total_count);

  /** @brief 分页查询内存分类详细历史数据。 */
  std::vector<MemDetailRecord> QueryMemDetail(const std::string& server_name,
                                              const TimeRange& time_range,
                                              int page, int page_size,
                                              int* total_count);

  /** @brief 分页查询逐核 SoftIRQ 详细历史数据。 */
  std::vector<SoftIrqDetailRecord> QuerySoftIrqDetail(
      const std::string& server_name, const TimeRange& time_range, int page,
      int page_size, int* total_count);

  /** @brief 查询持久化 incident，并可按根因/严重度过滤分页。 */
  std::vector<diagnostics::IncidentRecord> QueryIncidents(
      const std::string& server_name, const TimeRange& time_range,
      const std::string& root_cause, const std::string& severity, int page,
      int page_size, int* total_count);
  /** @brief 按 incident id 查询主记录及 detail。 */
  std::optional<diagnostics::IncidentRecord> QueryIncident(
      std::uint64_t incident_id);
  /** @brief 查询当前 active incident。 */
  std::vector<diagnostics::IncidentRecord> QueryActiveIncidents(
      const std::string& server_name);

 private:
  // 格式化时间为MySQL格式
  std::string FormatTime(const std::chrono::system_clock::time_point& tp) const;

  // 解析MySQL时间
  std::chrono::system_clock::time_point ParseTime(const char* str) const;

  // 执行查询并获取总数
  int GetTotalCount(const std::string& count_sql);

#ifdef ENABLE_MYSQL
  void LoadIncidentDetails(
      std::vector<diagnostics::IncidentRecord>* incidents);
#endif

#ifdef ENABLE_MYSQL
  MYSQL* conn_ = nullptr;
#endif
  std::mutex mtx_;
  bool initialized_ = false;
};

}  // namespace monitor
