#pragma once

#include <mutex>
#include <string>

#include "diagnostics/incident_store.h"

#ifdef ENABLE_MYSQL
#include <mysql/mysql.h>
#endif

namespace monitor::diagnostics {

// Owns the diagnostic schema and writes one durable incident snapshot per
// observation. The in-memory IncidentStore remains the fast-path fallback.
class DiagnosticPersistence {
 public:
  DiagnosticPersistence() = default;
  ~DiagnosticPersistence();

  DiagnosticPersistence(const DiagnosticPersistence&) = delete;
  DiagnosticPersistence& operator=(const DiagnosticPersistence&) = delete;

  bool Init(const std::string& host, const std::string& user,
            const std::string& password, const std::string& database);
  void Close();
  bool IsInitialized() const;
  bool Save(const IncidentRecord& incident);

 private:
#ifdef ENABLE_MYSQL
  bool EnsureSchema();
  bool Execute(const std::string& sql);
  std::string Escape(const std::string& value) const;
  std::string FormatTime(
      const std::chrono::system_clock::time_point& time) const;
  bool SaveEvidence(std::uint64_t incident_id,
                    const std::vector<Evidence>& evidence);

  MYSQL* connection_ = nullptr;
#endif
  mutable std::mutex mutex_;
  bool initialized_ = false;
};

}  // namespace monitor::diagnostics
