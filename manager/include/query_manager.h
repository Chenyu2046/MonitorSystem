#pragma once

#include <chrono>
#include <mutex>
#include <string>
#include <vector>

#ifdef ENABLE_MYSQL
#include <mysql/mysql.h>
#endif

namespace monitor {

// 排序方式
enum class SortOrder { DESC = 0, ASC = 1 };

// 服务器状态
enum class ServerStatus { ONLINE = 0, OFFLINE = 1 };

// 异常阈值配置
struct AnomalyThresholds {
  float cpu_threshold = 80.0f;
  float mem_threshold = 90.0f;
  float disk_threshold = 85.0f;
  float change_rate_threshold = 0.5f;  // 50%
};

// 时间范围
struct TimeRange {
  std::chrono::system_clock::time_point start_time;
  std::chrono::system_clock::time_point end_time;
};

// 性能数据记录
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
  float usr_percent_rate = 0;
  float system_percent_rate = 0;
  float io_wait_percent_rate = 0;
  float mem_used_percent_rate = 0;
  float disk_util_percent_rate = 0;
  float load_avg_1_rate = 0;
  float load_avg_3_rate = 0;
  float load_avg_15_rate = 0;
  float send_rate_rate = 0;
  float rcv_rate_rate = 0;
};

// 异常记录
struct AnomalyRecord {
  std::string server_name;
  std::chrono::system_clock::time_point timestamp;
  std::string anomaly_type;  // CPU_HIGH, MEM_HIGH, DISK_HIGH, RATE_SPIKE
  std::string severity;      // WARNING, CRITICAL
  float value = 0;
  float threshold = 0;
  std::string metric_name;
};

// 服务器评分摘要
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

// 集群统计信息
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


// 网络详细数据
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

// 磁盘详细数据
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

// 内存详细数据
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

// 软中断详细数据
struct SoftIrqDetailRecord {
  std::string server_name;
  std::string cpu_name;
  std::chrono::system_clock::time_point timestamp;
  int64_t hi = 0;
  int64_t timer = 0;
  int64_t net_tx = 0;
  int64_t net_rx = 0;
  int64_t block = 0;
  int64_t sched = 0;
};

// 查询管理器 - 封装MySQL查询逻辑
class QueryManager {
 public:
  // 所有 Query* 的 query_ok 表示数据库查询是否真正成功执行；空结果仍为 true，
  // 未编译 MySQL 支持、断连或 SQL 失败则为 false，供 RPC 层区分“无数据”和“不可用”。
  QueryManager();
  ~QueryManager();

  // 初始化MySQL连接
  bool Init(const std::string& host, const std::string& user,
            const std::string& password, const std::string& database);

  // 关闭连接
  void Close();

  // 验证时间范围
  bool ValidateTimeRange(const TimeRange& range) const;

  // 时间段性能数据查询
  std::vector<PerformanceRecord> QueryPerformance(const std::string& server_name,
                                                   const TimeRange& time_range,
                                                   int page, int page_size,
                                                   int* total_count,
                                                   bool* query_ok);

  // 变化率趋势查询（支持聚合）
  std::vector<PerformanceRecord> QueryTrend(const std::string& server_name,
                                             const TimeRange& time_range,
                                             int interval_seconds,
                                             bool* query_ok);

  // 异常数据查询
  std::vector<AnomalyRecord> QueryAnomaly(const std::string& server_name,
                                           const TimeRange& time_range,
                                           const AnomalyThresholds& thresholds,
                                           int page, int page_size,
                                           int* total_count, bool* query_ok);

  // 评分排序查询
  std::vector<ServerScoreSummary> QueryScoreRank(SortOrder order, int page,
                                                  int page_size,
                                                  int* total_count,
                                                  bool* query_ok);

  // 最新评分查询
  std::vector<ServerScoreSummary> QueryLatestScore(ClusterStats* stats,
                                                    bool* query_ok);

  // 详细数据查询
  std::vector<NetDetailRecord> QueryNetDetail(const std::string& server_name,
                                               const TimeRange& time_range,
                                               int page, int page_size,
                                               int* total_count, bool* query_ok);

  std::vector<DiskDetailRecord> QueryDiskDetail(const std::string& server_name,
                                                 const TimeRange& time_range,
                                                 int page, int page_size,
                                                 int* total_count, bool* query_ok);

  std::vector<MemDetailRecord> QueryMemDetail(const std::string& server_name,
                                               const TimeRange& time_range,
                                               int page, int page_size,
                                               int* total_count, bool* query_ok);

  std::vector<SoftIrqDetailRecord> QuerySoftIrqDetail(
      const std::string& server_name, const TimeRange& time_range, int page,
      int page_size, int* total_count, bool* query_ok);

 private:
  // 格式化时间为MySQL格式
  std::string FormatTime(const std::chrono::system_clock::time_point& tp) const;

  // 解析MySQL时间
  std::chrono::system_clock::time_point ParseTime(const char* str) const;

  // 执行查询并获取总数
  int GetTotalCount(const std::string& count_sql);

  std::string EscapeSql(const std::string& value) const;

#ifdef ENABLE_MYSQL
  bool EnsureConnectedLocked();
#endif

#ifdef ENABLE_MYSQL
  MYSQL* conn_ = nullptr;
  std::string database_host_;
  std::string database_user_;
  std::string database_password_;
  std::string database_name_;
#endif
  // 单个 MySQL 连接不允许并发使用；查询只在短暂窗口内等待此锁，防止断连重试
  // 把同步 RPC 工作线程无限排队。
  std::timed_mutex mtx_;
  bool initialized_ = false;
};

}  // namespace monitor
