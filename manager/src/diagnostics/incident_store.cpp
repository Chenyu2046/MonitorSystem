#include "diagnostics/incident_store.h"

#include <algorithm>
#include <utility>

namespace monitor::diagnostics {

IncidentStore::IncidentStore(std::size_t max_history)
    : max_history_(std::max<std::size_t>(1, max_history)) {}

std::optional<IncidentRecord> IncidentStore::Observe(
    const std::string& server_name, const std::string& state,
    const std::vector<Evidence>& evidence,
    const std::vector<RootCause>& root_causes,
    std::chrono::system_clock::time_point now) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto active = active_.find(server_name);
  if (root_causes.empty()) {
    if (active != active_.end()) {
      IncidentRecord closed = active->second;
      closed.end_time = now;
      closed.active = false;
      AddHistory(closed);
      active_.erase(active);
      return closed;
    }
    return std::nullopt;
  }

  const auto max_confidence =
      std::max_element(root_causes.begin(), root_causes.end(),
                       [](const RootCause& left, const RootCause& right) {
                         return left.confidence < right.confidence;
                       });
  const std::string severity = max_confidence->confidence >= 0.85   ? "CRITICAL"
                               : max_confidence->confidence >= 0.60 ? "HIGH"
                                                                    : "WARNING";

  if (active == active_.end()) {
    IncidentRecord incident;
    incident.id = next_id_++;
    incident.server_name = server_name;
    incident.severity = severity;
    incident.state = state;
    incident.start_time = now;
    incident.end_time = now;
    incident.root_causes = root_causes;
    incident.evidence = evidence;
    auto [it, inserted] = active_.emplace(server_name, std::move(incident));
    (void)inserted;
    return it->second;
  }

  active->second.severity = severity;
  active->second.active = true;
  active->second.state = state;
  active->second.end_time = now;
  active->second.root_causes = root_causes;
  active->second.evidence = evidence;
  return active->second;
}

std::vector<IncidentRecord> IncidentStore::List(
    const std::string& server_name,
    std::chrono::system_clock::time_point start_time,
    std::chrono::system_clock::time_point end_time,
    const std::string& root_cause, const std::string& severity) const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<IncidentRecord> result;
  for (const auto& incident : history_) {
    if (Matches(incident, server_name, start_time, end_time, root_cause,
                severity)) {
      result.push_back(incident);
    }
  }
  for (const auto& [host, incident] : active_) {
    if (Matches(incident, server_name, start_time, end_time, root_cause,
                severity)) {
      result.push_back(incident);
    }
  }
  std::sort(result.begin(), result.end(),
            [](const IncidentRecord& left, const IncidentRecord& right) {
              return left.start_time > right.start_time;
            });
  return result;
}

std::optional<IncidentRecord> IncidentStore::Get(std::uint64_t id) const {
  std::lock_guard<std::mutex> lock(mutex_);
  for (const auto& [host, incident] : active_) {
    if (incident.id == id) {
      return incident;
    }
  }
  for (const auto& incident : history_) {
    if (incident.id == id) {
      return incident;
    }
  }
  return std::nullopt;
}

std::vector<IncidentRecord> IncidentStore::Active(
    const std::string& server_name) const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<IncidentRecord> result;
  for (const auto& [host, incident] : active_) {
    if (server_name.empty() || host == server_name) {
      result.push_back(incident);
    }
  }
  return result;
}

bool IncidentStore::Matches(const IncidentRecord& incident,
                            const std::string& server_name,
                            std::chrono::system_clock::time_point start_time,
                            std::chrono::system_clock::time_point end_time,
                            const std::string& root_cause,
                            const std::string& severity) {
  if (!server_name.empty() && incident.server_name != server_name) {
    return false;
  }
  if (!severity.empty() && incident.severity != severity) {
    return false;
  }
  if (incident.end_time < start_time || incident.start_time > end_time) {
    return false;
  }
  if (!root_cause.empty()) {
    const auto match =
        std::find_if(incident.root_causes.begin(), incident.root_causes.end(),
                     [&root_cause](const RootCause& cause) {
                       return RootCauseTypeName(cause.type) == root_cause;
                     });
    if (match == incident.root_causes.end()) {
      return false;
    }
  }
  return true;
}

void IncidentStore::AddHistory(IncidentRecord incident) {
  history_.push_back(std::move(incident));
  while (history_.size() > max_history_) {
    history_.pop_front();
  }
}

}  // namespace monitor::diagnostics
