#pragma once

#include <chrono>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "diagnostics/evidence_builder.h"
#include "diagnostics/root_cause_engine.h"

namespace monitor::diagnostics {

struct IncidentRecord {
  std::uint64_t id = 0;
  std::string server_name;
  std::string severity;
  std::string state;
  bool active = true;
  std::chrono::system_clock::time_point start_time;
  std::chrono::system_clock::time_point end_time;
  std::vector<RootCause> root_causes;
  std::vector<Evidence> evidence;
};

class IncidentStore {
 public:
  explicit IncidentStore(std::size_t max_history = 256);

  // Returns the current incident when the observation changed incident state.
  // An empty result means there was no active incident to close and no new
  // incident to persist.
  std::optional<IncidentRecord> Observe(
      const std::string& server_name, const std::string& state,
      const std::vector<Evidence>& evidence,
      const std::vector<RootCause>& root_causes,
      std::chrono::system_clock::time_point now);

  std::vector<IncidentRecord> List(
      const std::string& server_name = {},
      std::chrono::system_clock::time_point start_time =
          std::chrono::system_clock::time_point::min(),
      std::chrono::system_clock::time_point end_time =
          std::chrono::system_clock::time_point::max(),
      const std::string& root_cause = {},
      const std::string& severity = {}) const;

  std::optional<IncidentRecord> Get(std::uint64_t id) const;
  std::vector<IncidentRecord> Active(const std::string& server_name = {}) const;

 private:
  static bool Matches(const IncidentRecord& incident,
                      const std::string& server_name,
                      std::chrono::system_clock::time_point start_time,
                      std::chrono::system_clock::time_point end_time,
                      const std::string& root_cause,
                      const std::string& severity);
  void AddHistory(IncidentRecord incident);

  const std::size_t max_history_;
  mutable std::mutex mutex_;
  std::uint64_t next_id_ = 1;
  std::unordered_map<std::string, IncidentRecord> active_;
  std::deque<IncidentRecord> history_;
};

}  // namespace monitor::diagnostics
