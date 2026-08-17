#include "diagnostics/diagnostic_persistence.h"

#include <algorithm>
#include <cstdlib>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <vector>

namespace monitor::diagnostics {

DiagnosticPersistence::~DiagnosticPersistence() { Close(); }

bool DiagnosticPersistence::Init(const std::string& host,
                                 const std::string& user,
                                 const std::string& password,
                                 const std::string& database) {
#ifdef ENABLE_MYSQL
  std::lock_guard<std::mutex> lock(mutex_);
  if (initialized_) {
    return true;
  }

  connection_ = mysql_init(nullptr);
  if (!connection_) {
    std::cerr << "DiagnosticPersistence: mysql_init failed" << std::endl;
    return false;
  }
  if (!mysql_real_connect(connection_, host.c_str(), user.c_str(),
                          password.c_str(), database.c_str(), 0, nullptr, 0)) {
    std::cerr << "DiagnosticPersistence: mysql_real_connect failed: "
              << mysql_error(connection_) << std::endl;
    mysql_close(connection_);
    connection_ = nullptr;
    return false;
  }
  mysql_set_character_set(connection_, "utf8mb4");
  if (!EnsureSchema()) {
    mysql_close(connection_);
    connection_ = nullptr;
    return false;
  }
  initialized_ = true;
  return true;
#else
  (void)host;
  (void)user;
  (void)password;
  (void)database;
  return false;
#endif
}

void DiagnosticPersistence::Close() {
#ifdef ENABLE_MYSQL
  std::lock_guard<std::mutex> lock(mutex_);
  if (connection_) {
    mysql_close(connection_);
    connection_ = nullptr;
  }
#endif
  initialized_ = false;
}

bool DiagnosticPersistence::IsInitialized() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return initialized_;
}

bool DiagnosticPersistence::Save(const IncidentRecord& incident) {
#ifdef ENABLE_MYSQL
  std::lock_guard<std::mutex> lock(mutex_);
  if (!initialized_ || !connection_ || incident.root_causes.empty()) {
    return false;
  }

  if (!Execute("START TRANSACTION")) {
    return false;
  }
  const auto rollback = [this] { Execute("ROLLBACK"); };

  const auto& cause = *std::max_element(
      incident.root_causes.begin(), incident.root_causes.end(),
      [](const RootCause& left, const RootCause& right) {
        return left.confidence < right.confidence;
      });
  const auto start_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            incident.start_time.time_since_epoch())
                            .count();
  const std::string incident_key =
      incident.server_name + ":" + std::to_string(start_ms);
  const std::string end_time =
      incident.active ? "NULL" : "'" + FormatTime(incident.end_time) + "'";

  std::ostringstream sql;
  sql << "INSERT INTO diagnostic_incident "
         "(incident_key, host_name, root_cause, severity, confidence, state, "
         "summary, start_time, end_time) VALUES ('"
      << Escape(incident_key) << "','" << Escape(incident.server_name) << "','"
      << Escape(RootCauseTypeName(cause.type)) << "','"
      << Escape(incident.severity) << "'," << cause.confidence << ",'"
      << Escape(incident.state) << "','" << Escape(cause.summary) << "','"
      << FormatTime(incident.start_time) << "'," << end_time
      << ") "
         "ON DUPLICATE KEY UPDATE id=LAST_INSERT_ID(id), "
         "root_cause=VALUES(root_cause), severity=VALUES(severity), "
         "confidence=VALUES(confidence), state=VALUES(state), "
         "summary=VALUES(summary), end_time=VALUES(end_time)";
  if (!Execute(sql.str())) {
    rollback();
    return false;
  }

  std::uint64_t incident_id = mysql_insert_id(connection_);
  if (incident_id == 0) {
    std::ostringstream lookup;
    lookup << "SELECT id FROM diagnostic_incident WHERE incident_key='"
           << Escape(incident_key) << "'";
    if (mysql_query(connection_, lookup.str().c_str()) != 0) {
      rollback();
      return false;
    }
    MYSQL_RES* result = mysql_store_result(connection_);
    if (!result) {
      rollback();
      return false;
    }
    MYSQL_ROW row = mysql_fetch_row(result);
    if (row && row[0]) {
      incident_id = std::strtoull(row[0], nullptr, 10);
    }
    mysql_free_result(result);
  }
  if (incident_id == 0 || !SaveRootCauses(incident_id, incident.root_causes) ||
      !SaveEvidence(incident_id, incident.evidence) || !Execute("COMMIT")) {
    rollback();
    return false;
  }
  return true;
#else
  (void)incident;
  return false;
#endif
}

#ifdef ENABLE_MYSQL
bool DiagnosticPersistence::EnsureSchema() {
  return Execute(
             "CREATE TABLE IF NOT EXISTS diagnostic_incident ("
             "id BIGINT AUTO_INCREMENT PRIMARY KEY,"
             "incident_key VARCHAR(512) NOT NULL UNIQUE,"
             "host_name VARCHAR(255) NOT NULL,"
             "root_cause VARCHAR(64) NOT NULL,"
             "severity VARCHAR(16) NOT NULL,"
             "confidence DOUBLE NOT NULL,"
             "state VARCHAR(32) NOT NULL,"
             "summary TEXT,"
             "start_time TIMESTAMP(3) NOT NULL,"
             "end_time TIMESTAMP(3) NULL,"
             "INDEX idx_host_time (host_name, start_time),"
             "INDEX idx_cause_time (root_cause, start_time))"
             " ENGINE=InnoDB DEFAULT CHARSET=utf8mb4") &&
          Execute(
              "CREATE TABLE IF NOT EXISTS diagnostic_evidence ("
             "id BIGINT AUTO_INCREMENT PRIMARY KEY,"
             "incident_id BIGINT NOT NULL,"
             "evidence_type VARCHAR(64) NOT NULL,"
             "source VARCHAR(64),"
             "target VARCHAR(255),"
             "metric VARCHAR(128),"
             "value DOUBLE,"
             "unit VARCHAR(32),"
             "severity DOUBLE,"
             "detail TEXT,"
             "event_time TIMESTAMP(3) NOT NULL,"
              "INDEX idx_incident (incident_id))"
              " ENGINE=InnoDB DEFAULT CHARSET=utf8mb4") &&
          Execute(
              "CREATE TABLE IF NOT EXISTS diagnostic_root_cause ("
              "id BIGINT AUTO_INCREMENT PRIMARY KEY,"
              "incident_id BIGINT NOT NULL,"
              "ordinal INT NOT NULL,"
              "root_cause VARCHAR(64) NOT NULL,"
              "confidence DOUBLE NOT NULL,"
              "evidence_ids TEXT NOT NULL,"
              "summary TEXT," 
              "UNIQUE KEY uk_incident_ordinal (incident_id, ordinal),"
              "INDEX idx_root_cause_incident (incident_id))"
              " ENGINE=InnoDB DEFAULT CHARSET=utf8mb4");
}

bool DiagnosticPersistence::Execute(const std::string& sql) {
  if (mysql_query(connection_, sql.c_str()) != 0) {
    std::cerr << "DiagnosticPersistence: query failed: "
              << mysql_error(connection_) << std::endl;
    return false;
  }
  return true;
}

std::string DiagnosticPersistence::Escape(const std::string& value) const {
  std::string escaped(value.size() * 2 + 1, '\0');
  const auto length = mysql_real_escape_string(connection_, escaped.data(),
                                               value.data(), value.size());
  escaped.resize(length);
  return escaped;
}

std::string DiagnosticPersistence::FormatTime(
    const std::chrono::system_clock::time_point& time) const {
  const auto millis_count =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          time.time_since_epoch())
          .count() %
      1000;
  const auto millis = std::chrono::milliseconds(millis_count);
  const auto seconds = time - millis;
  const std::time_t raw = std::chrono::system_clock::to_time_t(seconds);
  std::tm tm_time{};
  localtime_r(&raw, &tm_time);
  std::ostringstream output;
  output << std::put_time(&tm_time, "%Y-%m-%d %H:%M:%S") << '.'
         << std::setfill('0') << std::setw(3) << millis.count();
  return output.str();
}

bool DiagnosticPersistence::SaveRootCauses(
    std::uint64_t incident_id, const std::vector<RootCause>& root_causes) {
  std::ostringstream delete_sql;
  delete_sql << "DELETE FROM diagnostic_root_cause WHERE incident_id="
             << incident_id;
  if (!Execute(delete_sql.str())) {
    return false;
  }
  for (std::size_t ordinal = 0; ordinal < root_causes.size(); ++ordinal) {
    const auto& cause = root_causes[ordinal];
    std::ostringstream evidence_ids;
    for (std::size_t index = 0; index < cause.evidence_ids.size(); ++index) {
      if (index > 0) evidence_ids << '\n';
      evidence_ids << cause.evidence_ids[index];
    }
    std::ostringstream insert;
    insert << "INSERT INTO diagnostic_root_cause "
              "(incident_id, ordinal, root_cause, confidence, evidence_ids, "
              "summary) VALUES ("
           << incident_id << "," << ordinal << ", '"
           << Escape(RootCauseTypeName(cause.type)) << "',"
           << cause.confidence << ", '" << Escape(evidence_ids.str())
           << "', '" << Escape(cause.summary) << "')";
    if (!Execute(insert.str())) {
      return false;
    }
  }
  return true;
}

bool DiagnosticPersistence::SaveEvidence(
    std::uint64_t incident_id, const std::vector<Evidence>& evidence) {
  std::ostringstream delete_sql;
  delete_sql << "DELETE FROM diagnostic_evidence WHERE incident_id="
             << incident_id;
  if (!Execute(delete_sql.str())) {
    return false;
  }
  for (const auto& item : evidence) {
    std::ostringstream insert;
    insert << "INSERT INTO diagnostic_evidence "
              "(incident_id, evidence_type, source, target, metric, value, "
              "unit, severity, detail, event_time) VALUES ("
           << incident_id << ",'" << Escape(EvidenceTypeName(item.type))
           << "','" << Escape(item.source) << "','" << Escape(item.target)
           << "','" << Escape(item.id) << "'," << item.value << ",'"
           << Escape(item.unit) << "'," << item.severity << ",'"
           << Escape(item.detail) << "','" << FormatTime(item.timestamp)
           << "')";
    if (!Execute(insert.str())) {
      return false;
    }
  }
  return true;
}
#endif

}  // namespace monitor::diagnostics
