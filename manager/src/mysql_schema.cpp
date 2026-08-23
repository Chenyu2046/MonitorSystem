#include "mysql_schema.h"

#ifdef ENABLE_MYSQL

#include <iostream>
#include <string>

namespace monitor {
namespace {

bool QueryCountEquals(MYSQL* connection, const char* query,
                      const char* expected, const char* description) {
  if (!connection || mysql_query(connection, query) != 0) {
    if (connection) {
      std::cerr << description << " check failed: " << mysql_error(connection)
                << "\n";
    }
    return false;
  }
  MYSQL_RES* result = mysql_store_result(connection);
  if (!result) {
    std::cerr << description << " result unavailable: "
              << mysql_error(connection) << "\n";
    return false;
  }
  MYSQL_ROW row = mysql_fetch_row(result);
  const bool ready = row && row[0] && std::string(row[0]) == expected;
  mysql_free_result(result);
  if (!ready) std::cerr << description << " is incompatible\n";
  return ready;
}

}  // namespace

bool EnsureOrdinarySchemaReady(MYSQL* connection) {
  constexpr char kTableQuery[] =
      "SELECT COUNT(DISTINCT table_name) FROM information_schema.tables "
      "WHERE table_schema = DATABASE() AND table_name IN "
      "('server_performance','server_net_detail','server_disk_detail',"
      "'server_mem_detail','server_softirq_detail')";
  constexpr char kSoftIrqTypeQuery[] =
      "SELECT COUNT(DISTINCT column_name) FROM information_schema.columns "
      "WHERE table_schema = DATABASE() "
      "AND table_name = 'server_softirq_detail' "
      "AND column_name IN "
      "('hi','timer','net_tx','net_rx','block','irq_poll','tasklet','sched',"
      "'hrtimer','rcu') AND data_type IN ('float','double')";
  constexpr char kHealthNumericQuery[] =
      "SELECT COUNT(DISTINCT column_name) FROM information_schema.columns "
      "WHERE table_schema = DATABASE() AND table_name = 'server_performance' "
      "AND column_name IN ('health_score','resource_score','anomaly_score',"
      "'anomaly_rate_5m','confidence') AND data_type IN ('float','double')";
  constexpr char kHealthTextQuery[] =
      "SELECT COUNT(DISTINCT column_name) FROM information_schema.columns "
      "WHERE table_schema = DATABASE() AND table_name = 'server_performance' "
      "AND ((column_name IN ('health_state','health_model_state') "
      "AND data_type = 'varchar') OR (column_name = 'health_top_signals' "
      "AND data_type IN ('text','mediumtext','longtext')))";
  constexpr char kHealthValidQuery[] =
      "SELECT COUNT(*) FROM information_schema.columns "
      "WHERE table_schema = DATABASE() AND table_name = 'server_performance' "
      "AND column_name = 'health_valid' AND data_type = 'tinyint'";
  return QueryCountEquals(connection, kTableQuery, "5",
                          "ordinary MySQL table schema") &&
         QueryCountEquals(connection, kSoftIrqTypeQuery, "10",
                          "SoftIRQ rate column schema") &&
         QueryCountEquals(connection, kHealthNumericQuery, "5",
                          "health numeric column schema") &&
         QueryCountEquals(connection, kHealthTextQuery, "3",
                          "health text column schema") &&
         QueryCountEquals(connection, kHealthValidQuery, "1",
                          "health validity column schema");
}

}  // namespace monitor
#endif
