/**
 * @file query_manager.cpp
 * @brief MySQL 历史查询、时间/分页转换和 incident detail 解析实现。
 *
 * 数据流为 QueryService protobuf request -> 参数校验/分页 -> MySQL 主表
 * 或 detail 表 -> C++ record -> response protobuf。所有查询共享连接互斥，
 * MySQL disabled 时保留空结果语义，实时 incident 则由 HostManager fallback。
 */

#include "query_manager.h"
#include "health/top_signal_codec.h"
#include "mysql_schema.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <cstdlib>

#include <ctime>
#include <iomanip>
#include <iostream>
#include <sstream>

namespace monitor {

#ifdef ENABLE_MYSQL
namespace {
bool ParseFiniteDouble(const char* text, double* value) {
  if (!text || !value || *text == '\0') return false;
  errno = 0;
  char* end = nullptr;
  const double parsed = std::strtod(text, &end);
  if (errno == ERANGE || end == text || *end != '\0' ||
      !std::isfinite(parsed)) {
    return false;
  }
  *value = parsed;
  return true;
}

bool ParseHealthColumns(MYSQL_ROW row, int* index,
                        PerformanceRecord* record) {
  double values[5]{};
  bool valid = true;
  for (double& value : values) {
    valid = ParseFiniteDouble(row[*index], &value) && valid;
    ++*index;
  }
  record->health_score = static_cast<float>(values[0]);
  record->resource_score = static_cast<float>(values[1]);
  record->anomaly_score = static_cast<float>(values[2]);
  record->anomaly_rate_5m = static_cast<float>(values[3]);
  record->confidence = static_cast<float>(values[4]);
  record->state = row[*index] ? row[*index] : "";
  ++*index;
  record->model_state = row[*index] ? row[*index] : "";
  ++*index;
  const bool stored_valid = row[*index] && std::string(row[*index]) == "1";
  ++*index;
  record->top_signals =
      health::DecodeTopSignals(row[*index] ? row[*index] : "");
  ++*index;
  record->health_valid = stored_valid && valid && !record->state.empty() &&
                         !record->model_state.empty();
  return record->health_valid;
}

bool ParseHealthColumns(MYSQL_ROW row, int* index,
                        ServerScoreSummary* record) {
  double values[5]{};
  bool valid = true;
  for (double& value : values) {
    valid = ParseFiniteDouble(row[*index], &value) && valid;
    ++*index;
  }
  record->health_score = static_cast<float>(values[0]);
  record->resource_score = static_cast<float>(values[1]);
  record->anomaly_score = static_cast<float>(values[2]);
  record->anomaly_rate_5m = static_cast<float>(values[3]);
  record->confidence = static_cast<float>(values[4]);
  record->state = row[*index] ? row[*index] : "";
  ++*index;
  record->model_state = row[*index] ? row[*index] : "";
  ++*index;
  const bool stored_valid = row[*index] && std::string(row[*index]) == "1";
  ++*index;
  record->top_signals =
      health::DecodeTopSignals(row[*index] ? row[*index] : "");
  ++*index;
  record->health_valid = stored_valid && valid && !record->state.empty() &&
                         !record->model_state.empty();
  return record->health_valid;
}

/** @brief 使用当前 MySQL 连接转义查询字符串。 */
std::string EscapeSql(MYSQL* connection, const std::string& value) {
  std::string escaped(value.size() * 2 + 1, '\0');
  const auto length = mysql_real_escape_string(
      connection, escaped.data(), value.data(), value.size());
  escaped.resize(length);
  return escaped;
}

/** @brief 将数据库 root_cause 名称还原为内部枚举。 */
diagnostics::RootCauseType ParseRootCauseType(const std::string& name) {
  if (name == "CPU_SATURATION")
    return diagnostics::RootCauseType::kCpuSaturation;
  if (name == "DISK_IO_SATURATION")
    return diagnostics::RootCauseType::kDiskIoSaturation;
  if (name == "NETWORK_STACK_PRESSURE")
    return diagnostics::RootCauseType::kNetworkStackPressure;
  if (name == "MEMORY_PRESSURE")
    return diagnostics::RootCauseType::kMemoryPressure;
  if (name == "LOCK_CONTENTION")
    return diagnostics::RootCauseType::kLockContention;
  return diagnostics::RootCauseType::kUnknown;
}

/** @brief 将数据库 evidence_type 名称还原为内部枚举。 */
diagnostics::EvidenceType ParseEvidenceType(const std::string& name) {
  if (name == "cpu_usage") return diagnostics::EvidenceType::kCpuUsage;
  if (name == "run_queue") return diagnostics::EvidenceType::kRunQueue;
  if (name == "io_wait") return diagnostics::EvidenceType::kIoWait;
  if (name == "disk_util") return diagnostics::EvidenceType::kDiskUtil;
  if (name == "disk_latency") return diagnostics::EvidenceType::kDiskLatency;
  if (name == "bpf_block_latency")
    return diagnostics::EvidenceType::kBpfBlockLatency;
  if (name == "net_pps") return diagnostics::EvidenceType::kNetPps;
  if (name == "tcp_retrans") return diagnostics::EvidenceType::kTcpRetrans;
  if (name == "softirq_net_rx")
    return diagnostics::EvidenceType::kSoftirqNetRx;
  if (name == "scheduler_switches")
    return diagnostics::EvidenceType::kSchedulerSwitches;
  if (name == "scheduler_wakeups")
    return diagnostics::EvidenceType::kSchedulerWakeups;
  if (name == "diagnostic_capability_degraded")
    return diagnostics::EvidenceType::kDiagnosticCapabilityDegraded;
  if (name == "memory_available")
    return diagnostics::EvidenceType::kMemoryAvailable;
  if (name == "oncpu_stack") return diagnostics::EvidenceType::kOnCpuStack;
  if (name == "offcpu_stack") return diagnostics::EvidenceType::kOffCpuStack;
  if (name == "lock_wait_stack")
    return diagnostics::EvidenceType::kLockWaitStack;
  if (name == "health_anomaly_signal")
    return diagnostics::EvidenceType::kHealthAnomalySignal;
  return diagnostics::EvidenceType::kCpuUsage;
}

/** @brief 将持久化的换行分隔 evidence id 还原为 vector。 */
std::vector<std::string> SplitEvidenceIds(const char* value) {
  std::vector<std::string> ids;
  if (!value) return ids;
  std::istringstream input(value);
  std::string id;
  while (std::getline(input, id)) {
    if (!id.empty()) ids.push_back(std::move(id));
  }
  return ids;
}
}  // namespace
#endif

QueryManager::QueryManager() = default;

QueryManager::~QueryManager() { Close(); }

bool QueryManager::Init(const std::string& host, const std::string& user,
                        const std::string& password,
                        const std::string& database) {
  // 连接初始化和 initialized_ 状态在同一把 mutex 下完成；查询调用者看
  // 到 false 时应返回空结果或走内存 fallback。
#ifdef ENABLE_MYSQL
  std::lock_guard<std::mutex> lock(mtx_);
  if (initialized_) {
    return true;
  }

  conn_ = mysql_init(nullptr);
  if (!conn_) {
    std::cerr << "QueryManager: mysql_init failed" << std::endl;
    return false;
  }

  if (!mysql_real_connect(conn_, host.c_str(), user.c_str(), password.c_str(),
                          database.c_str(), 0, nullptr, 0)) {
    std::cerr << "QueryManager: mysql_real_connect failed: "
              << mysql_error(conn_) << std::endl;
    mysql_close(conn_);
    conn_ = nullptr;
    return false;
  }

  // 设置字符集
  mysql_set_character_set(conn_, "utf8mb4");
  if (!EnsureOrdinarySchemaReady(conn_)) {
    std::cerr << "QueryManager: incompatible ordinary schema" << std::endl;
    mysql_close(conn_);
    conn_ = nullptr;
    return false;
  }
  initialized_ = true;
  std::cout << "QueryManager: MySQL connection initialized" << std::endl;
  return true;
#else
  (void)host;
  (void)user;
  (void)password;
  (void)database;
  std::cerr << "QueryManager: MySQL support not enabled" << std::endl;
  return false;
#endif
}

void QueryManager::Close() {
#ifdef ENABLE_MYSQL
  std::lock_guard<std::mutex> lock(mtx_);
  if (conn_) {
    mysql_close(conn_);
    conn_ = nullptr;
  }
  initialized_ = false;
#endif
}

bool QueryManager::IsInitialized() {
  std::lock_guard<std::mutex> lock(mtx_);
  return initialized_;
}

bool QueryManager::ValidateTimeRange(const TimeRange& range) const {
  // 所有历史查询共用这一边界检查，避免 SQL 时间条件反转。
  return range.start_time <= range.end_time;
}

std::string QueryManager::FormatTime(
    const std::chrono::system_clock::time_point& tp) const {
  // MySQL timestamp 保留毫秒；查询 API 的 Timestamp 当前只转换秒，
  // 这里保持数据库既有毫秒格式兼容。
  const auto millis_count =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          tp.time_since_epoch())
          .count() %
      1000;
  const auto millis = std::chrono::milliseconds(millis_count);
  const auto seconds = tp - millis;
  std::time_t t = std::chrono::system_clock::to_time_t(seconds);
  std::tm tm_time{};
  localtime_r(&t, &tm_time);
  std::ostringstream output;
  output << std::put_time(&tm_time, "%Y-%m-%d %H:%M:%S") << '.'
         << std::setfill('0') << std::setw(3) << millis.count();
  return output.str();
}

std::chrono::system_clock::time_point QueryManager::ParseTime(
    const char* str) const {
  std::tm tm = {};
  std::istringstream ss(str);
  ss >> std::get_time(&tm, "%Y-%m-%d %H:%M:%S");
  std::chrono::milliseconds millis(0);
  if (ss.peek() == '.') {
    ss.get();
    std::string fraction;
    ss >> fraction;
    fraction.resize(3, '0');
    millis = std::chrono::milliseconds(std::stoi(fraction.substr(0, 3)));
  }
  return std::chrono::system_clock::from_time_t(std::mktime(&tm)) + millis;
}

int QueryManager::GetTotalCount(const std::string& count_sql) {
  // 分页查询需要独立 COUNT；调用方持有 mtx_，因此此函数不再重复加锁。
#ifdef ENABLE_MYSQL
  if (mysql_query(conn_, count_sql.c_str()) != 0) {
    std::cerr << "QueryManager: count query failed: " << mysql_error(conn_)
              << std::endl;
    return 0;
  }

  MYSQL_RES* result = mysql_store_result(conn_);
  if (!result) {
    return 0;
  }

  int count = 0;
  MYSQL_ROW row = mysql_fetch_row(result);
  if (row && row[0]) {
    count = std::atoi(row[0]);
  }
  mysql_free_result(result);
  return count;
#else
  (void)count_sql;
  return 0;
#endif
}

std::vector<PerformanceRecord> QueryManager::QueryPerformance(
    const std::string& server_name, const TimeRange& time_range, int page,
    int page_size, int* total_count) {
  std::vector<PerformanceRecord> records;

  // 主表查询同时返回本轮值和变化率，并通过 total_count/page/page_size
  // 支持 QueryService 的分页 response。

#ifdef ENABLE_MYSQL
  std::lock_guard<std::mutex> lock(mtx_);
  if (!initialized_ || !conn_) {
    return records;
  }

  // 验证参数
  if (!ValidateTimeRange(time_range)) {
    std::cerr << "QueryManager: Invalid time range" << std::endl;
    return records;
  }
  if (page < 1) page = 1;
  if (page_size < 1) page_size = 100;

  std::string start_time = FormatTime(time_range.start_time);
  std::string end_time = FormatTime(time_range.end_time);

  // 获取总数
  std::ostringstream count_sql;
  count_sql << "SELECT COUNT(*) FROM server_performance WHERE server_name='"
            << server_name << "' AND timestamp BETWEEN '" << start_time
            << "' AND '" << end_time << "'";
  if (total_count) {
    *total_count = GetTotalCount(count_sql.str());
  }

  // 查询数据
  int offset = (page - 1) * page_size;
  std::ostringstream sql;
  sql << "SELECT server_name, timestamp, cpu_percent, usr_percent, "
         "system_percent, nice_percent, idle_percent, io_wait_percent, "
         "irq_percent, soft_irq_percent, load_avg_1, load_avg_3, load_avg_15, "
         "mem_used_percent, total, free, avail, disk_util_percent, "
         "send_rate, rcv_rate, score, health_score, resource_score, "
         "anomaly_score, anomaly_rate_5m, confidence, health_state, "
         "health_model_state, health_valid, health_top_signals, "
         "cpu_percent_rate, "
         "mem_used_percent_rate, "
         "disk_util_percent_rate, load_avg_1_rate, send_rate_rate, rcv_rate_rate "
         "FROM server_performance WHERE server_name='"
      << server_name << "' AND timestamp BETWEEN '" << start_time << "' AND '"
      << end_time << "' ORDER BY timestamp DESC LIMIT " << page_size
      << " OFFSET " << offset;

  if (mysql_query(conn_, sql.str().c_str()) != 0) {
    std::cerr << "QueryManager: query failed: " << mysql_error(conn_)
              << std::endl;
    return records;
  }

  MYSQL_RES* result = mysql_store_result(conn_);
  if (!result) {
    return records;
  }

  MYSQL_ROW row;
  while ((row = mysql_fetch_row(result))) {
    PerformanceRecord rec;
    int i = 0;
    rec.server_name = row[i++] ? row[i - 1] : "";
    rec.timestamp = row[i] ? ParseTime(row[i]) : std::chrono::system_clock::now();
    i++;
    rec.cpu_percent = row[i] ? std::atof(row[i]) : 0; i++;
    rec.usr_percent = row[i] ? std::atof(row[i]) : 0; i++;
    rec.system_percent = row[i] ? std::atof(row[i]) : 0; i++;
    rec.nice_percent = row[i] ? std::atof(row[i]) : 0; i++;
    rec.idle_percent = row[i] ? std::atof(row[i]) : 0; i++;
    rec.io_wait_percent = row[i] ? std::atof(row[i]) : 0; i++;
    rec.irq_percent = row[i] ? std::atof(row[i]) : 0; i++;
    rec.soft_irq_percent = row[i] ? std::atof(row[i]) : 0; i++;
    rec.load_avg_1 = row[i] ? std::atof(row[i]) : 0; i++;
    rec.load_avg_3 = row[i] ? std::atof(row[i]) : 0; i++;
    rec.load_avg_15 = row[i] ? std::atof(row[i]) : 0; i++;
    rec.mem_used_percent = row[i] ? std::atof(row[i]) : 0; i++;
    rec.mem_total = row[i] ? std::atof(row[i]) : 0; i++;
    rec.mem_free = row[i] ? std::atof(row[i]) : 0; i++;
    rec.mem_avail = row[i] ? std::atof(row[i]) : 0; i++;
    rec.disk_util_percent = row[i] ? std::atof(row[i]) : 0; i++;
    rec.send_rate = row[i] ? std::atof(row[i]) : 0; i++;
    rec.rcv_rate = row[i] ? std::atof(row[i]) : 0; i++;
    rec.score = row[i] ? std::atof(row[i]) : 0; i++;
    ParseHealthColumns(row, &i, &rec);
    rec.cpu_percent_rate = row[i] ? std::atof(row[i]) : 0; i++;
    rec.mem_used_percent_rate = row[i] ? std::atof(row[i]) : 0; i++;
    rec.disk_util_percent_rate = row[i] ? std::atof(row[i]) : 0; i++;
    rec.load_avg_1_rate = row[i] ? std::atof(row[i]) : 0; i++;
    rec.send_rate_rate = row[i] ? std::atof(row[i]) : 0; i++;
    rec.rcv_rate_rate = row[i] ? std::atof(row[i]) : 0;
    records.push_back(rec);
  }
  mysql_free_result(result);
#else
  (void)server_name;
  (void)time_range;
  (void)page;
  (void)page_size;
  (void)total_count;
#endif

  return records;
}


std::vector<PerformanceRecord> QueryManager::QueryTrend(
    const std::string& server_name, const TimeRange& time_range,
    int interval_seconds) {
  std::vector<PerformanceRecord> records;

  // interval_seconds>0 时按时间桶 AVG 聚合；0 表示返回原始时间序列。

#ifdef ENABLE_MYSQL
  std::lock_guard<std::mutex> lock(mtx_);
  if (!initialized_ || !conn_) {
    return records;
  }

  if (!ValidateTimeRange(time_range)) {
    std::cerr << "QueryManager: Invalid time range" << std::endl;
    return records;
  }

  std::string start_time = FormatTime(time_range.start_time);
  std::string end_time = FormatTime(time_range.end_time);

  std::ostringstream sql;
  if (interval_seconds > 0) {
    // 带聚合的查询
    sql << "SELECT server_name, "
           "FROM_UNIXTIME(FLOOR(UNIX_TIMESTAMP(timestamp) / "
        << interval_seconds << ") * " << interval_seconds
        << ") as time_bucket, "
           "AVG(cpu_percent) as cpu_percent, "
           "AVG(usr_percent) as usr_percent, "
           "AVG(system_percent) as system_percent, "
           "AVG(io_wait_percent) as io_wait_percent, "
           "AVG(load_avg_1) as load_avg_1, "
           "AVG(load_avg_3) as load_avg_3, "
           "AVG(load_avg_15) as load_avg_15, "
           "AVG(mem_used_percent) as mem_used_percent, "
           "AVG(disk_util_percent) as disk_util_percent, "
           "AVG(send_rate) as send_rate, "
           "AVG(rcv_rate) as rcv_rate, "
           "AVG(score) as score, "
           "AVG(health_score) as health_score, "
           "AVG(resource_score) as resource_score, "
           "AVG(anomaly_score) as anomaly_score, "
           "AVG(anomaly_rate_5m) as anomaly_rate_5m, "
           "AVG(confidence) as confidence, "
           "SUBSTRING_INDEX(GROUP_CONCAT(health_state ORDER BY timestamp "
           "DESC), ',', 1) as health_state, "
           "SUBSTRING_INDEX(GROUP_CONCAT(health_model_state ORDER BY "
           "timestamp DESC), ',', 1) as health_model_state, "
           "MIN(health_valid) as health_valid, "
           "SUBSTRING_INDEX(GROUP_CONCAT(health_top_signals ORDER BY "
           "timestamp DESC), ',', 1) as health_top_signals, "
           "AVG(cpu_percent_rate) as cpu_percent_rate, "
           "AVG(mem_used_percent_rate) as mem_used_percent_rate, "
           "AVG(disk_util_percent_rate) as disk_util_percent_rate, "
           "AVG(load_avg_1_rate) as load_avg_1_rate "
           "FROM server_performance WHERE server_name='"
        << server_name << "' AND timestamp BETWEEN '" << start_time
        << "' AND '" << end_time
        << "' GROUP BY server_name, time_bucket ORDER BY time_bucket";
  } else {
    // 不聚合，直接查询
    sql << "SELECT server_name, timestamp, cpu_percent, usr_percent, "
           "system_percent, io_wait_percent, load_avg_1, load_avg_3, "
           "load_avg_15, mem_used_percent, disk_util_percent, send_rate, "
           "rcv_rate, score, health_score, resource_score, anomaly_score, "
           "anomaly_rate_5m, confidence, health_state, health_model_state, "
           "health_valid, health_top_signals, cpu_percent_rate, "
           "mem_used_percent_rate, "
           "disk_util_percent_rate, load_avg_1_rate "
           "FROM server_performance WHERE server_name='"
        << server_name << "' AND timestamp BETWEEN '" << start_time
        << "' AND '" << end_time << "' ORDER BY timestamp";
  }

  if (mysql_query(conn_, sql.str().c_str()) != 0) {
    std::cerr << "QueryManager: trend query failed: " << mysql_error(conn_)
              << std::endl;
    return records;
  }

  MYSQL_RES* result = mysql_store_result(conn_);
  if (!result) {
    return records;
  }

  MYSQL_ROW row;
  while ((row = mysql_fetch_row(result))) {
    PerformanceRecord rec;
    int i = 0;
    rec.server_name = row[i++] ? row[i - 1] : "";
    rec.timestamp = row[i] ? ParseTime(row[i]) : std::chrono::system_clock::now();
    i++;
    rec.cpu_percent = row[i] ? std::atof(row[i]) : 0; i++;
    rec.usr_percent = row[i] ? std::atof(row[i]) : 0; i++;
    rec.system_percent = row[i] ? std::atof(row[i]) : 0; i++;
    rec.io_wait_percent = row[i] ? std::atof(row[i]) : 0; i++;
    rec.load_avg_1 = row[i] ? std::atof(row[i]) : 0; i++;
    rec.load_avg_3 = row[i] ? std::atof(row[i]) : 0; i++;
    rec.load_avg_15 = row[i] ? std::atof(row[i]) : 0; i++;
    rec.mem_used_percent = row[i] ? std::atof(row[i]) : 0; i++;
    rec.disk_util_percent = row[i] ? std::atof(row[i]) : 0; i++;
    rec.send_rate = row[i] ? std::atof(row[i]) : 0; i++;
    rec.rcv_rate = row[i] ? std::atof(row[i]) : 0; i++;
    rec.score = row[i] ? std::atof(row[i]) : 0; i++;
    ParseHealthColumns(row, &i, &rec);
    rec.cpu_percent_rate = row[i] ? std::atof(row[i]) : 0; i++;
    rec.mem_used_percent_rate = row[i] ? std::atof(row[i]) : 0; i++;
    rec.disk_util_percent_rate = row[i] ? std::atof(row[i]) : 0; i++;
    rec.load_avg_1_rate = row[i] ? std::atof(row[i]) : 0;
    records.push_back(rec);
  }
  mysql_free_result(result);
#else
  (void)server_name;
  (void)time_range;
  (void)interval_seconds;
#endif

  return records;
}


std::vector<AnomalyRecord> QueryManager::QueryAnomaly(
    const std::string& server_name, const TimeRange& time_range,
    const AnomalyThresholds& thresholds, int page, int page_size,
    int* total_count) {
  std::vector<AnomalyRecord> records;

  // 异常查询在数据库侧按阈值筛选；它与 Worker 实时 anomaly state 是查询
  // 历史视图，不改变 Worker 的状态机。

#ifdef ENABLE_MYSQL
  std::lock_guard<std::mutex> lock(mtx_);
  if (!initialized_ || !conn_) {
    return records;
  }

  if (!ValidateTimeRange(time_range)) {
    std::cerr << "QueryManager: Invalid time range" << std::endl;
    return records;
  }
  if (page < 1) page = 1;
  if (page_size < 1) page_size = 100;

  std::string start_time = FormatTime(time_range.start_time);
  std::string end_time = FormatTime(time_range.end_time);

  // 构建WHERE条件
  std::ostringstream where_clause;
  where_clause << "timestamp BETWEEN '" << start_time << "' AND '" << end_time
               << "'";
  if (!server_name.empty()) {
    where_clause << " AND server_name='" << server_name << "'";
  }
  where_clause << " AND (cpu_percent > " << thresholds.cpu_threshold
               << " OR mem_used_percent > " << thresholds.mem_threshold
               << " OR disk_util_percent > " << thresholds.disk_threshold
               << " OR ABS(cpu_percent_rate) > " << thresholds.change_rate_threshold
               << " OR ABS(mem_used_percent_rate) > " << thresholds.change_rate_threshold
               << ")";

  // 获取总数
  std::ostringstream count_sql;
  count_sql << "SELECT COUNT(*) FROM server_performance WHERE "
            << where_clause.str();
  if (total_count) {
    *total_count = GetTotalCount(count_sql.str());
  }

  // 查询数据
  int offset = (page - 1) * page_size;
  std::ostringstream sql;
  sql << "SELECT server_name, timestamp, cpu_percent, mem_used_percent, "
         "disk_util_percent, cpu_percent_rate, mem_used_percent_rate "
         "FROM server_performance WHERE "
      << where_clause.str() << " ORDER BY timestamp DESC LIMIT " << page_size
      << " OFFSET " << offset;

  if (mysql_query(conn_, sql.str().c_str()) != 0) {
    std::cerr << "QueryManager: anomaly query failed: " << mysql_error(conn_)
              << std::endl;
    return records;
  }

  MYSQL_RES* result = mysql_store_result(conn_);
  if (!result) {
    return records;
  }

  MYSQL_ROW row;
  while ((row = mysql_fetch_row(result))) {
    std::string srv_name = row[0] ? row[0] : "";
    auto ts = row[1] ? ParseTime(row[1]) : std::chrono::system_clock::now();
    float cpu = row[2] ? std::atof(row[2]) : 0;
    float mem = row[3] ? std::atof(row[3]) : 0;
    float disk = row[4] ? std::atof(row[4]) : 0;
    float cpu_rate = row[5] ? std::atof(row[5]) : 0;
    float mem_rate = row[6] ? std::atof(row[6]) : 0;

    // 生成异常记录
    auto add_anomaly = [&](const std::string& type, const std::string& metric,
                           float value, float threshold) {
      AnomalyRecord rec;
      rec.server_name = srv_name;
      rec.timestamp = ts;
      rec.anomaly_type = type;
      rec.metric_name = metric;
      rec.value = value;
      rec.threshold = threshold;
      // 判断严重程度
      if (type == "CPU_HIGH" && value > 95) {
        rec.severity = "CRITICAL";
      } else if (type == "MEM_HIGH" && value > 95) {
        rec.severity = "CRITICAL";
      } else if (type == "DISK_HIGH" && value > 95) {
        rec.severity = "CRITICAL";
      } else if (type == "RATE_SPIKE" && std::abs(value) > 1.0) {
        rec.severity = "CRITICAL";
      } else {
        rec.severity = "WARNING";
      }
      records.push_back(rec);
    };

    if (cpu > thresholds.cpu_threshold) {
      add_anomaly("CPU_HIGH", "cpu_percent", cpu, thresholds.cpu_threshold);
    }
    if (mem > thresholds.mem_threshold) {
      add_anomaly("MEM_HIGH", "mem_used_percent", mem, thresholds.mem_threshold);
    }
    if (disk > thresholds.disk_threshold) {
      add_anomaly("DISK_HIGH", "disk_util_percent", disk, thresholds.disk_threshold);
    }
    if (std::abs(cpu_rate) > thresholds.change_rate_threshold) {
      add_anomaly("RATE_SPIKE", "cpu_percent_rate", cpu_rate,
                  thresholds.change_rate_threshold);
    }
    if (std::abs(mem_rate) > thresholds.change_rate_threshold) {
      add_anomaly("RATE_SPIKE", "mem_used_percent_rate", mem_rate,
                  thresholds.change_rate_threshold);
    }
  }
  mysql_free_result(result);
#else
  (void)server_name;
  (void)time_range;
  (void)thresholds;
  (void)page;
  (void)page_size;
  (void)total_count;
#endif

  return records;
}


std::vector<ServerScoreSummary> QueryManager::QueryScoreRank(
    SortOrder order, ScoreKind score_kind, int page, int page_size,
    int* total_count) {
  std::vector<ServerScoreSummary> records;

#ifdef ENABLE_MYSQL
  std::lock_guard<std::mutex> lock(mtx_);
  if (!initialized_ || !conn_) {
    return records;
  }

  if (page < 1) page = 1;
  if (page_size < 1) page_size = 100;

  // 获取总数（不同服务器数量）
  std::string count_sql;
  if (score_kind == ScoreKind::HEALTH) {
    count_sql =
        "SELECT COUNT(*) FROM server_performance p1 INNER JOIN ("
        "SELECT server_name, MAX(timestamp) AS max_ts FROM "
        "server_performance GROUP BY server_name) p2 ON "
        "p1.server_name=p2.server_name AND p1.timestamp=p2.max_ts "
        "WHERE p1.health_valid=1";
  } else {
    count_sql =
        "SELECT COUNT(DISTINCT server_name) FROM server_performance";
  }
  if (total_count) {
    *total_count = GetTotalCount(count_sql);
  }

  // 查询每台服务器的最新数据并排序
  int offset = (page - 1) * page_size;
  std::string order_str = (order == SortOrder::ASC) ? "ASC" : "DESC";
  const char* order_column =
      score_kind == ScoreKind::HEALTH ? "p1.health_score"
                                      : "p1.resource_score";

  std::ostringstream sql;
  sql << "SELECT p1.server_name, p1.score, p1.health_score, "
         "p1.resource_score, p1.anomaly_score, p1.anomaly_rate_5m, "
         "p1.confidence, p1.health_state, p1.health_model_state, "
         "p1.health_valid, p1.health_top_signals, p1.timestamp, "
         "p1.cpu_percent, "
         "p1.mem_used_percent, p1.disk_util_percent, p1.load_avg_1 "
         "FROM server_performance p1 "
         "INNER JOIN ("
         "  SELECT server_name, MAX(timestamp) as max_ts "
         "  FROM server_performance GROUP BY server_name"
         ") p2 ON p1.server_name = p2.server_name AND p1.timestamp = p2.max_ts "
      << (score_kind == ScoreKind::HEALTH ? "WHERE p1.health_valid=1 " : "")
      << "ORDER BY " << order_column << " " << order_str << " LIMIT "
      << page_size << " OFFSET " << offset;

  if (mysql_query(conn_, sql.str().c_str()) != 0) {
    std::cerr << "QueryManager: score rank query failed: " << mysql_error(conn_)
              << std::endl;
    return records;
  }

  MYSQL_RES* result = mysql_store_result(conn_);
  if (!result) {
    return records;
  }

  auto now = std::chrono::system_clock::now();
  MYSQL_ROW row;
  while ((row = mysql_fetch_row(result))) {
    ServerScoreSummary rec;
    int i = 0;
    rec.server_name = row[i] ? row[i] : "";
    ++i;
    rec.score = row[i] ? std::atof(row[i]) : 0;
    ++i;
    ParseHealthColumns(row, &i, &rec);
    rec.last_update = row[i] ? ParseTime(row[i]) : now;
    ++i;
    rec.cpu_percent = row[i] ? std::atof(row[i]) : 0;
    ++i;
    rec.mem_used_percent = row[i] ? std::atof(row[i]) : 0;
    ++i;
    rec.disk_util_percent = row[i] ? std::atof(row[i]) : 0;
    ++i;
    rec.load_avg_1 = row[i] ? std::atof(row[i]) : 0;

    // 判断在线状态（60秒阈值）
    auto age = std::chrono::duration_cast<std::chrono::seconds>(
                   now - rec.last_update)
                   .count();
    rec.status = (age > 60) ? ServerStatus::OFFLINE : ServerStatus::ONLINE;

    records.push_back(rec);
  }
  mysql_free_result(result);
#else
  (void)order;
  (void)score_kind;
  (void)page;
  (void)page_size;
  (void)total_count;
#endif

  return records;
}


std::vector<ServerScoreSummary> QueryManager::QueryLatestScore(
    ClusterStats* stats) {
  std::vector<ServerScoreSummary> records;

#ifdef ENABLE_MYSQL
  std::lock_guard<std::mutex> lock(mtx_);
  if (!initialized_ || !conn_) {
    return records;
  }

  // 查询每台服务器的最新数据
  std::string sql =
      "SELECT p1.server_name, p1.score, p1.health_score, p1.resource_score, "
      "p1.anomaly_score, p1.anomaly_rate_5m, p1.confidence, "
      "p1.health_state, p1.health_model_state, p1.health_valid, "
      "p1.health_top_signals, "
      "p1.timestamp, p1.cpu_percent, "
      "p1.mem_used_percent, p1.disk_util_percent, p1.load_avg_1 "
      "FROM server_performance p1 "
      "INNER JOIN ("
      "  SELECT server_name, MAX(timestamp) as max_ts "
      "  FROM server_performance GROUP BY server_name"
      ") p2 ON p1.server_name = p2.server_name AND p1.timestamp = p2.max_ts "
      "ORDER BY p1.score DESC";

  if (mysql_query(conn_, sql.c_str()) != 0) {
    std::cerr << "QueryManager: latest score query failed: "
              << mysql_error(conn_) << std::endl;
    return records;
  }

  MYSQL_RES* result = mysql_store_result(conn_);
  if (!result) {
    return records;
  }

  auto now = std::chrono::system_clock::now();
  float total_score = 0;
  float max_score = -1;
  float min_score = 101;
  std::string best_server, worst_server;
  float total_health_score = 0;
  float max_health_score = -1;
  float min_health_score = 101;
  int valid_health_count = 0;
  std::string healthiest_server, least_healthy_server;
  int online_count = 0, offline_count = 0;

  MYSQL_ROW row;
  while ((row = mysql_fetch_row(result))) {
    ServerScoreSummary rec;
    int i = 0;
    rec.server_name = row[i] ? row[i] : "";
    ++i;
    rec.score = row[i] ? std::atof(row[i]) : 0;
    ++i;
    ParseHealthColumns(row, &i, &rec);
    rec.last_update = row[i] ? ParseTime(row[i]) : now;
    ++i;
    rec.cpu_percent = row[i] ? std::atof(row[i]) : 0;
    ++i;
    rec.mem_used_percent = row[i] ? std::atof(row[i]) : 0;
    ++i;
    rec.disk_util_percent = row[i] ? std::atof(row[i]) : 0;
    ++i;
    rec.load_avg_1 = row[i] ? std::atof(row[i]) : 0;

    // 判断在线状态（60秒阈值）
    auto age = std::chrono::duration_cast<std::chrono::seconds>(
                   now - rec.last_update)
                   .count();
    rec.status = (age > 60) ? ServerStatus::OFFLINE : ServerStatus::ONLINE;

    if (rec.status == ServerStatus::ONLINE) {
      online_count++;
    } else {
      offline_count++;
    }

    // 统计
    total_score += rec.score;
    if (rec.score > max_score) {
      max_score = rec.score;
      best_server = rec.server_name;
    }
    if (rec.score < min_score) {
      min_score = rec.score;
      worst_server = rec.server_name;
    }
    if (rec.health_valid) {
      total_health_score += rec.health_score;
      ++valid_health_count;
      if (rec.health_score > max_health_score) {
        max_health_score = rec.health_score;
        healthiest_server = rec.server_name;
      }
      if (rec.health_score < min_health_score) {
        min_health_score = rec.health_score;
        least_healthy_server = rec.server_name;
      }
    }

    records.push_back(rec);
  }
  mysql_free_result(result);

  // 填充集群统计
  if (stats) {
    stats->total_servers = static_cast<int>(records.size());
    stats->online_servers = online_count;
    stats->offline_servers = offline_count;
    stats->avg_score = records.empty() ? 0 : total_score / records.size();
    stats->max_score = max_score > 0 ? max_score : 0;
    stats->min_score = min_score < 101 ? min_score : 0;
    stats->best_server = best_server;
    stats->worst_server = worst_server;
    stats->avg_health_score =
        valid_health_count == 0 ? 0 : total_health_score / valid_health_count;
    stats->max_health_score =
        max_health_score >= 0 ? max_health_score : 0;
    stats->min_health_score =
        min_health_score <= 100 ? min_health_score : 0;
    stats->healthiest_server = healthiest_server;
    stats->least_healthy_server = least_healthy_server;
  }
#else
  (void)stats;
#endif

  return records;
}


std::vector<NetDetailRecord> QueryManager::QueryNetDetail(
    const std::string& server_name, const TimeRange& time_range, int page,
    int page_size, int* total_count) {
  std::vector<NetDetailRecord> records;

#ifdef ENABLE_MYSQL
  std::lock_guard<std::mutex> lock(mtx_);
  if (!initialized_ || !conn_) {
    return records;
  }

  if (!ValidateTimeRange(time_range)) {
    return records;
  }
  if (page < 1) page = 1;
  if (page_size < 1) page_size = 100;

  std::string start_time = FormatTime(time_range.start_time);
  std::string end_time = FormatTime(time_range.end_time);

  // 获取总数
  std::ostringstream count_sql;
  count_sql << "SELECT COUNT(*) FROM server_net_detail WHERE server_name='"
            << server_name << "' AND timestamp BETWEEN '" << start_time
            << "' AND '" << end_time << "'";
  if (total_count) {
    *total_count = GetTotalCount(count_sql.str());
  }

  // 查询数据
  int offset = (page - 1) * page_size;
  std::ostringstream sql;
  sql << "SELECT server_name, net_name, timestamp, err_in, err_out, "
         "drop_in, drop_out, rcv_bytes_rate, snd_bytes_rate, "
         "rcv_packets_rate, snd_packets_rate "
         "FROM server_net_detail WHERE server_name='"
      << server_name << "' AND timestamp BETWEEN '" << start_time << "' AND '"
      << end_time << "' ORDER BY timestamp DESC LIMIT " << page_size
      << " OFFSET " << offset;

  if (mysql_query(conn_, sql.str().c_str()) != 0) {
    std::cerr << "QueryManager: net detail query failed: " << mysql_error(conn_)
              << std::endl;
    return records;
  }

  MYSQL_RES* result = mysql_store_result(conn_);
  if (!result) {
    return records;
  }

  MYSQL_ROW row;
  while ((row = mysql_fetch_row(result))) {
    NetDetailRecord rec;
    int i = 0;
    rec.server_name = row[i++] ? row[i - 1] : "";
    rec.net_name = row[i++] ? row[i - 1] : "";
    rec.timestamp = row[i] ? ParseTime(row[i]) : std::chrono::system_clock::now();
    i++;
    rec.err_in = row[i] ? std::stoull(row[i]) : 0; i++;
    rec.err_out = row[i] ? std::stoull(row[i]) : 0; i++;
    rec.drop_in = row[i] ? std::stoull(row[i]) : 0; i++;
    rec.drop_out = row[i] ? std::stoull(row[i]) : 0; i++;
    rec.rcv_bytes_rate = row[i] ? std::atof(row[i]) : 0; i++;
    rec.snd_bytes_rate = row[i] ? std::atof(row[i]) : 0; i++;
    rec.rcv_packets_rate = row[i] ? std::atof(row[i]) : 0; i++;
    rec.snd_packets_rate = row[i] ? std::atof(row[i]) : 0;
    records.push_back(rec);
  }
  mysql_free_result(result);
#else
  (void)server_name;
  (void)time_range;
  (void)page;
  (void)page_size;
  (void)total_count;
#endif

  return records;
}

std::vector<DiskDetailRecord> QueryManager::QueryDiskDetail(
    const std::string& server_name, const TimeRange& time_range, int page,
    int page_size, int* total_count) {
  std::vector<DiskDetailRecord> records;

#ifdef ENABLE_MYSQL
  std::lock_guard<std::mutex> lock(mtx_);
  if (!initialized_ || !conn_) {
    return records;
  }

  if (!ValidateTimeRange(time_range)) {
    return records;
  }
  if (page < 1) page = 1;
  if (page_size < 1) page_size = 100;

  std::string start_time = FormatTime(time_range.start_time);
  std::string end_time = FormatTime(time_range.end_time);

  // 获取总数
  std::ostringstream count_sql;
  count_sql << "SELECT COUNT(*) FROM server_disk_detail WHERE server_name='"
            << server_name << "' AND timestamp BETWEEN '" << start_time
            << "' AND '" << end_time << "'";
  if (total_count) {
    *total_count = GetTotalCount(count_sql.str());
  }

  // 查询数据
  int offset = (page - 1) * page_size;
  std::ostringstream sql;
  sql << "SELECT server_name, disk_name, timestamp, read_bytes_per_sec, "
         "write_bytes_per_sec, read_iops, write_iops, avg_read_latency_ms, "
         "avg_write_latency_ms, util_percent "
         "FROM server_disk_detail WHERE server_name='"
      << server_name << "' AND timestamp BETWEEN '" << start_time << "' AND '"
      << end_time << "' ORDER BY timestamp DESC LIMIT " << page_size
      << " OFFSET " << offset;

  if (mysql_query(conn_, sql.str().c_str()) != 0) {
    std::cerr << "QueryManager: disk detail query failed: "
              << mysql_error(conn_) << std::endl;
    return records;
  }

  MYSQL_RES* result = mysql_store_result(conn_);
  if (!result) {
    return records;
  }

  MYSQL_ROW row;
  while ((row = mysql_fetch_row(result))) {
    DiskDetailRecord rec;
    int i = 0;
    rec.server_name = row[i++] ? row[i - 1] : "";
    rec.disk_name = row[i++] ? row[i - 1] : "";
    rec.timestamp = row[i] ? ParseTime(row[i]) : std::chrono::system_clock::now();
    i++;
    rec.read_bytes_per_sec = row[i] ? std::atof(row[i]) : 0; i++;
    rec.write_bytes_per_sec = row[i] ? std::atof(row[i]) : 0; i++;
    rec.read_iops = row[i] ? std::atof(row[i]) : 0; i++;
    rec.write_iops = row[i] ? std::atof(row[i]) : 0; i++;
    rec.avg_read_latency_ms = row[i] ? std::atof(row[i]) : 0; i++;
    rec.avg_write_latency_ms = row[i] ? std::atof(row[i]) : 0; i++;
    rec.util_percent = row[i] ? std::atof(row[i]) : 0;
    records.push_back(rec);
  }
  mysql_free_result(result);
#else
  (void)server_name;
  (void)time_range;
  (void)page;
  (void)page_size;
  (void)total_count;
#endif

  return records;
}

std::vector<MemDetailRecord> QueryManager::QueryMemDetail(
    const std::string& server_name, const TimeRange& time_range, int page,
    int page_size, int* total_count) {
  std::vector<MemDetailRecord> records;

#ifdef ENABLE_MYSQL
  std::lock_guard<std::mutex> lock(mtx_);
  if (!initialized_ || !conn_) {
    return records;
  }

  if (!ValidateTimeRange(time_range)) {
    return records;
  }
  if (page < 1) page = 1;
  if (page_size < 1) page_size = 100;

  std::string start_time = FormatTime(time_range.start_time);
  std::string end_time = FormatTime(time_range.end_time);

  // 获取总数
  std::ostringstream count_sql;
  count_sql << "SELECT COUNT(*) FROM server_mem_detail WHERE server_name='"
            << server_name << "' AND timestamp BETWEEN '" << start_time
            << "' AND '" << end_time << "'";
  if (total_count) {
    *total_count = GetTotalCount(count_sql.str());
  }

  // 查询数据
  int offset = (page - 1) * page_size;
  std::ostringstream sql;
  sql << "SELECT server_name, timestamp, total, free, avail, buffers, "
         "cached, active, inactive, dirty "
         "FROM server_mem_detail WHERE server_name='"
      << server_name << "' AND timestamp BETWEEN '" << start_time << "' AND '"
      << end_time << "' ORDER BY timestamp DESC LIMIT " << page_size
      << " OFFSET " << offset;

  if (mysql_query(conn_, sql.str().c_str()) != 0) {
    std::cerr << "QueryManager: mem detail query failed: " << mysql_error(conn_)
              << std::endl;
    return records;
  }

  MYSQL_RES* result = mysql_store_result(conn_);
  if (!result) {
    return records;
  }

  MYSQL_ROW row;
  while ((row = mysql_fetch_row(result))) {
    MemDetailRecord rec;
    int i = 0;
    rec.server_name = row[i++] ? row[i - 1] : "";
    rec.timestamp = row[i] ? ParseTime(row[i]) : std::chrono::system_clock::now();
    i++;
    rec.total = row[i] ? std::atof(row[i]) : 0; i++;
    rec.free = row[i] ? std::atof(row[i]) : 0; i++;
    rec.avail = row[i] ? std::atof(row[i]) : 0; i++;
    rec.buffers = row[i] ? std::atof(row[i]) : 0; i++;
    rec.cached = row[i] ? std::atof(row[i]) : 0; i++;
    rec.active = row[i] ? std::atof(row[i]) : 0; i++;
    rec.inactive = row[i] ? std::atof(row[i]) : 0; i++;
    rec.dirty = row[i] ? std::atof(row[i]) : 0;
    records.push_back(rec);
  }
  mysql_free_result(result);
#else
  (void)server_name;
  (void)time_range;
  (void)page;
  (void)page_size;
  (void)total_count;
#endif

  return records;
}

std::vector<SoftIrqDetailRecord> QueryManager::QuerySoftIrqDetail(
    const std::string& server_name, const TimeRange& time_range, int page,
    int page_size, int* total_count) {
  std::vector<SoftIrqDetailRecord> records;

#ifdef ENABLE_MYSQL
  std::lock_guard<std::mutex> lock(mtx_);
  if (!initialized_ || !conn_) {
    return records;
  }

  if (!ValidateTimeRange(time_range)) {
    return records;
  }
  if (page < 1) page = 1;
  if (page_size < 1) page_size = 100;

  std::string start_time = FormatTime(time_range.start_time);
  std::string end_time = FormatTime(time_range.end_time);

  // 获取总数
  std::ostringstream count_sql;
  count_sql << "SELECT COUNT(*) FROM server_softirq_detail WHERE server_name='"
            << server_name << "' AND timestamp BETWEEN '" << start_time
            << "' AND '" << end_time << "'";
  if (total_count) {
    *total_count = GetTotalCount(count_sql.str());
  }

  // 查询数据
  int offset = (page - 1) * page_size;
  std::ostringstream sql;
  sql << "SELECT server_name, cpu_name, timestamp, hi, timer, net_tx, "
         "net_rx, block, irq_poll, tasklet, sched, hrtimer, rcu "
         "FROM server_softirq_detail WHERE server_name='"
      << server_name << "' AND timestamp BETWEEN '" << start_time << "' AND '"
      << end_time << "' ORDER BY timestamp DESC LIMIT " << page_size
      << " OFFSET " << offset;

  if (mysql_query(conn_, sql.str().c_str()) != 0) {
    std::cerr << "QueryManager: softirq detail query failed: "
              << mysql_error(conn_) << std::endl;
    return records;
  }

  MYSQL_RES* result = mysql_store_result(conn_);
  if (!result) {
    return records;
  }

  MYSQL_ROW row;
  while ((row = mysql_fetch_row(result))) {
    SoftIrqDetailRecord rec;
    int i = 0;
    rec.server_name = row[i++] ? row[i - 1] : "";
    rec.cpu_name = row[i++] ? row[i - 1] : "";
    if (!row[i]) continue;
    rec.timestamp = ParseTime(row[i]);
    i++;
    float* values[] = {&rec.hi,      &rec.timer,  &rec.net_tx, &rec.net_rx,
                       &rec.block,   &rec.irq_poll, &rec.tasklet,
                       &rec.sched,   &rec.hrtimer, &rec.rcu};
    bool valid = true;
    for (float* value : values) {
      double parsed = 0;
      if (!row[i] || !ParseFiniteDouble(row[i], &parsed)) {
        valid = false;
      } else {
        *value = static_cast<float>(parsed);
      }
      ++i;
    }
    if (!valid) continue;
    records.push_back(rec);
  }
  mysql_free_result(result);
#else
  (void)server_name;
  (void)time_range;
  (void)page;
  (void)page_size;
  (void)total_count;
#endif

  return records;
}

std::vector<diagnostics::IncidentRecord> QueryManager::QueryIncidents(
    const std::string& server_name, const TimeRange& time_range,
    const std::string& root_cause, const std::string& severity, int page,
    int page_size, int* total_count) {
  std::vector<diagnostics::IncidentRecord> records;
#ifdef ENABLE_MYSQL
  std::lock_guard<std::mutex> lock(mtx_);
  if (!initialized_ || !conn_) {
    if (total_count) *total_count = 0;
    return records;
  }

  std::ostringstream where;
  if (!server_name.empty()) {
    where << " AND host_name='" << EscapeSql(conn_, server_name) << "'";
  }
  if (time_range.end_time != std::chrono::system_clock::time_point::max()) {
    where << " AND start_time <= '" << FormatTime(time_range.end_time) << "'";
  }
  if (time_range.start_time != std::chrono::system_clock::time_point::min()) {
    where << " AND (end_time IS NULL OR end_time >= '"
          << FormatTime(time_range.start_time) << "')";
  }
  if (!root_cause.empty()) {
    const auto escaped_root_cause = EscapeSql(conn_, root_cause);
    where << " AND (root_cause='" << escaped_root_cause
          << "' OR EXISTS (SELECT 1 FROM diagnostic_root_cause rc WHERE "
             "rc.incident_id=diagnostic_incident.id AND rc.root_cause='"
          << escaped_root_cause << "'))";
  }
  if (!severity.empty()) {
    where << " AND severity='" << EscapeSql(conn_, severity) << "'";
  }

  const std::string base = " FROM diagnostic_incident WHERE 1=1" + where.str();
  if (total_count) {
    *total_count = GetTotalCount("SELECT COUNT(*)" + base);
  }
  page = std::max(1, page);
  page_size = std::clamp(page_size, 1, 1000);
  const int offset = (page - 1) * page_size;
  std::ostringstream sql;
  sql << "SELECT id, host_name, severity, state, start_time, end_time, "
         "root_cause, confidence, summary"
      << base << " ORDER BY start_time DESC LIMIT " << page_size << " OFFSET "
      << offset;
  if (mysql_query(conn_, sql.str().c_str()) != 0) {
    std::cerr << "QueryManager: incident query failed: " << mysql_error(conn_)
              << std::endl;
    return records;
  }
  MYSQL_RES* result = mysql_store_result(conn_);
  if (!result) return records;
  MYSQL_ROW row;
  while ((row = mysql_fetch_row(result))) {
    diagnostics::IncidentRecord incident;
    incident.id = row[0] ? std::strtoull(row[0], nullptr, 10) : 0;
    incident.server_name = row[1] ? row[1] : "";
    incident.severity = row[2] ? row[2] : "";
    incident.state = row[3] ? row[3] : "";
    incident.start_time =
        row[4] ? ParseTime(row[4]) : std::chrono::system_clock::time_point{};
    incident.end_time = row[5] ? ParseTime(row[5]) : incident.start_time;
    incident.active = row[5] == nullptr;
    diagnostics::RootCause cause;
    cause.type = diagnostics::RootCauseType::kUnknown;
    const std::string cause_name = row[6] ? row[6] : "";
    const std::array<std::pair<const char*, diagnostics::RootCauseType>, 5>
        cause_names{
            {{"CPU_SATURATION", diagnostics::RootCauseType::kCpuSaturation},
             {"DISK_IO_SATURATION",
              diagnostics::RootCauseType::kDiskIoSaturation},
             {"NETWORK_STACK_PRESSURE",
              diagnostics::RootCauseType::kNetworkStackPressure},
             {"MEMORY_PRESSURE", diagnostics::RootCauseType::kMemoryPressure},
             {"LOCK_CONTENTION", diagnostics::RootCauseType::kLockContention}}};
    for (const auto& [name, type] : cause_names) {
      if (cause_name == name) {
        cause.type = type;
        break;
      }
    }
    cause.confidence = row[7] ? std::atof(row[7]) : 0.0;
    cause.summary = row[8] ? row[8] : "";
    incident.root_causes.push_back(std::move(cause));
    records.push_back(std::move(incident));
  }
  mysql_free_result(result);
  for (auto& incident : records) {
    std::ostringstream evidence_sql;
    evidence_sql << "SELECT evidence_type, source, target, metric, value, "
                    "unit, severity, detail, event_time FROM "
                    "diagnostic_evidence WHERE incident_id="
                 << incident.id << " ORDER BY id";
    if (mysql_query(conn_, evidence_sql.str().c_str()) != 0) continue;
    MYSQL_RES* evidence_result = mysql_store_result(conn_);
    if (!evidence_result) continue;
    MYSQL_ROW evidence_row;
    while ((evidence_row = mysql_fetch_row(evidence_result))) {
      diagnostics::Evidence evidence;
      evidence.type = diagnostics::EvidenceType::kCpuUsage;
      const std::string type_name = evidence_row[0] ? evidence_row[0] : "";
      const std::array<std::pair<const char*, diagnostics::EvidenceType>, 16>
          evidence_names{
              {{"cpu_usage", diagnostics::EvidenceType::kCpuUsage},
               {"run_queue", diagnostics::EvidenceType::kRunQueue},
               {"io_wait", diagnostics::EvidenceType::kIoWait},
               {"disk_util", diagnostics::EvidenceType::kDiskUtil},
               {"disk_latency", diagnostics::EvidenceType::kDiskLatency},
               {"bpf_block_latency",
                diagnostics::EvidenceType::kBpfBlockLatency},
               {"net_pps", diagnostics::EvidenceType::kNetPps},
               {"tcp_retrans", diagnostics::EvidenceType::kTcpRetrans},
               {"softirq_net_rx", diagnostics::EvidenceType::kSoftirqNetRx},
               {"scheduler_switches",
                diagnostics::EvidenceType::kSchedulerSwitches},
               {"scheduler_wakeups",
                diagnostics::EvidenceType::kSchedulerWakeups},
               {"diagnostic_capability_degraded",
                diagnostics::EvidenceType::kDiagnosticCapabilityDegraded},
               {"memory_available",
                diagnostics::EvidenceType::kMemoryAvailable},
               {"oncpu_stack", diagnostics::EvidenceType::kOnCpuStack},
               {"offcpu_stack", diagnostics::EvidenceType::kOffCpuStack},
               {"lock_wait_stack",
                diagnostics::EvidenceType::kLockWaitStack}}};
      for (const auto& [name, type] : evidence_names) {
        if (type_name == name) {
          evidence.type = type;
          break;
        }
      }
      evidence.source = evidence_row[1] ? evidence_row[1] : "";
      evidence.target = evidence_row[2] ? evidence_row[2] : "";
      evidence.id = evidence_row[3] ? evidence_row[3] : "";
      evidence.value = evidence_row[4] ? std::atof(evidence_row[4]) : 0.0;
      evidence.unit = evidence_row[5] ? evidence_row[5] : "";
      evidence.severity = evidence_row[6] ? std::atof(evidence_row[6]) : 0.0;
      evidence.detail = evidence_row[7] ? evidence_row[7] : "";
      evidence.timestamp =
          evidence_row[8] ? ParseTime(evidence_row[8]) : incident.start_time;
      incident.evidence.push_back(std::move(evidence));
    }
    mysql_free_result(evidence_result);
  }
  LoadIncidentDetails(&records);
#else
  (void)server_name;
  (void)time_range;
  (void)root_cause;
  (void)severity;
  (void)page;
  (void)page_size;
  if (total_count) *total_count = 0;
#endif
  return records;
}

#ifdef ENABLE_MYSQL
void QueryManager::LoadIncidentDetails(
    std::vector<diagnostics::IncidentRecord>* incidents) {
  for (auto& incident : *incidents) {
    std::ostringstream root_sql;
    root_sql << "SELECT root_cause, confidence, evidence_ids, summary "
                "FROM diagnostic_root_cause WHERE incident_id="
             << incident.id << " ORDER BY ordinal";
    if (mysql_query(conn_, root_sql.str().c_str()) == 0) {
      MYSQL_RES* root_result = mysql_store_result(conn_);
      if (root_result) {
        std::vector<diagnostics::RootCause> root_causes;
        MYSQL_ROW root_row;
        while ((root_row = mysql_fetch_row(root_result))) {
          diagnostics::RootCause cause;
          cause.type = ParseRootCauseType(root_row[0] ? root_row[0] : "");
          cause.confidence = root_row[1] ? std::atof(root_row[1]) : 0.0;
          cause.evidence_ids = SplitEvidenceIds(root_row[2]);
          cause.summary = root_row[3] ? root_row[3] : "";
          root_causes.push_back(std::move(cause));
        }
        mysql_free_result(root_result);
        if (!root_causes.empty()) {
          incident.root_causes = std::move(root_causes);
        }
      }
    }

    if (!incident.evidence.empty()) continue;
    std::ostringstream evidence_sql;
    evidence_sql << "SELECT evidence_type, source, target, metric, value, "
                    "unit, severity, detail, event_time FROM "
                    "diagnostic_evidence WHERE incident_id="
                 << incident.id << " ORDER BY id";
    if (mysql_query(conn_, evidence_sql.str().c_str()) != 0) continue;
    MYSQL_RES* evidence_result = mysql_store_result(conn_);
    if (!evidence_result) continue;
    MYSQL_ROW evidence_row;
    while ((evidence_row = mysql_fetch_row(evidence_result))) {
      diagnostics::Evidence evidence;
      evidence.type = ParseEvidenceType(evidence_row[0] ? evidence_row[0] : "");
      evidence.source = evidence_row[1] ? evidence_row[1] : "";
      evidence.target = evidence_row[2] ? evidence_row[2] : "";
      evidence.id = evidence_row[3] ? evidence_row[3] : "";
      evidence.value = evidence_row[4] ? std::atof(evidence_row[4]) : 0.0;
      evidence.unit = evidence_row[5] ? evidence_row[5] : "";
      evidence.severity = evidence_row[6] ? std::atof(evidence_row[6]) : 0.0;
      evidence.detail = evidence_row[7] ? evidence_row[7] : "";
      evidence.timestamp = evidence_row[8]
                               ? ParseTime(evidence_row[8])
                               : incident.start_time;
      incident.evidence.push_back(std::move(evidence));
    }
    mysql_free_result(evidence_result);
  }
}
#endif

std::optional<diagnostics::IncidentRecord> QueryManager::QueryIncident(
    std::uint64_t incident_id) {
#ifdef ENABLE_MYSQL
  std::lock_guard<std::mutex> lock(mtx_);
  if (!initialized_ || !conn_) return std::nullopt;
  std::ostringstream sql;
  sql << "SELECT id, host_name, severity, state, start_time, end_time, "
         "root_cause, confidence, summary FROM diagnostic_incident WHERE id="
      << incident_id;
  if (mysql_query(conn_, sql.str().c_str()) != 0) return std::nullopt;
  MYSQL_RES* result = mysql_store_result(conn_);
  if (!result) return std::nullopt;
  MYSQL_ROW row = mysql_fetch_row(result);
  if (!row) {
    mysql_free_result(result);
    return std::nullopt;
  }
  diagnostics::IncidentRecord incident;
  incident.id = row[0] ? std::strtoull(row[0], nullptr, 10) : 0;
  incident.server_name = row[1] ? row[1] : "";
  incident.severity = row[2] ? row[2] : "";
  incident.state = row[3] ? row[3] : "";
  incident.start_time = row[4] ? ParseTime(row[4])
                               : std::chrono::system_clock::time_point{};
  incident.end_time = row[5] ? ParseTime(row[5]) : incident.start_time;
  incident.active = row[5] == nullptr;
  diagnostics::RootCause cause;
  cause.type = ParseRootCauseType(row[6] ? row[6] : "");
  cause.confidence = row[7] ? std::atof(row[7]) : 0.0;
  cause.summary = row[8] ? row[8] : "";
  incident.root_causes.push_back(std::move(cause));
  mysql_free_result(result);
  std::vector<diagnostics::IncidentRecord> records;
  records.push_back(std::move(incident));
  LoadIncidentDetails(&records);
  return records.front();
#else
  (void)incident_id;
  return std::nullopt;
#endif
}

std::vector<diagnostics::IncidentRecord> QueryManager::QueryActiveIncidents(
    const std::string& server_name) {
  std::vector<diagnostics::IncidentRecord> records;
#ifdef ENABLE_MYSQL
  std::lock_guard<std::mutex> lock(mtx_);
  if (!initialized_ || !conn_) return records;
  std::ostringstream sql;
  sql << "SELECT id, host_name, severity, state, start_time, root_cause, "
         "confidence, summary FROM diagnostic_incident WHERE end_time IS NULL";
  if (!server_name.empty()) {
    sql << " AND host_name='" << EscapeSql(conn_, server_name) << "'";
  }
  sql << " ORDER BY start_time DESC";
  if (mysql_query(conn_, sql.str().c_str()) != 0) return records;
  MYSQL_RES* result = mysql_store_result(conn_);
  if (!result) return records;
  MYSQL_ROW row;
  while ((row = mysql_fetch_row(result))) {
    diagnostics::IncidentRecord incident;
    incident.id = row[0] ? std::strtoull(row[0], nullptr, 10) : 0;
    incident.server_name = row[1] ? row[1] : "";
    incident.severity = row[2] ? row[2] : "";
    incident.state = row[3] ? row[3] : "";
    incident.start_time =
        row[4] ? ParseTime(row[4]) : std::chrono::system_clock::time_point{};
    incident.end_time = incident.start_time;
    incident.active = true;
    diagnostics::RootCause cause;
    cause.confidence = row[6] ? std::atof(row[6]) : 0.0;
    cause.summary = row[7] ? row[7] : "";
    cause.type = diagnostics::RootCauseType::kUnknown;
    const std::string cause_name = row[5] ? row[5] : "";
    if (cause_name == "CPU_SATURATION")
      cause.type = diagnostics::RootCauseType::kCpuSaturation;
    if (cause_name == "DISK_IO_SATURATION")
      cause.type = diagnostics::RootCauseType::kDiskIoSaturation;
    if (cause_name == "NETWORK_STACK_PRESSURE")
      cause.type = diagnostics::RootCauseType::kNetworkStackPressure;
    if (cause_name == "MEMORY_PRESSURE")
      cause.type = diagnostics::RootCauseType::kMemoryPressure;
    if (cause_name == "LOCK_CONTENTION")
      cause.type = diagnostics::RootCauseType::kLockContention;
    incident.root_causes.push_back(std::move(cause));
    records.push_back(std::move(incident));
  }
  mysql_free_result(result);
  LoadIncidentDetails(&records);
#else
  (void)server_name;
#endif
  return records;
}

}  // namespace monitor
