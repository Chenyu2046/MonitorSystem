#include "monitor/mem_monitor.h"

#include <charconv>
#include <cmath>
#include <fstream>
#include <limits>
#include <map>
#include <sstream>
#include <string_view>

namespace monitor {

namespace {
bool ParseNonnegative(std::string_view token, std::int64_t* value) {
  if (!value || token.empty()) return false;
  std::uint64_t parsed = 0;
  const auto result = std::from_chars(token.data(), token.data() + token.size(),
                                      parsed);
  if (result.ec != std::errc() ||
      result.ptr != token.data() + token.size() ||
      parsed > static_cast<std::uint64_t>(
                   std::numeric_limits<std::int64_t>::max())) {
    return false;
  }
  *value = static_cast<std::int64_t>(parsed);
  return true;
}
}  // namespace

bool MemMonitor::Init() {
  std::ifstream file("/proc/meminfo");
  return file.good();
}

CollectStatus MemMonitor::UpdateOnce(
    monitor::proto::MonitorInfo* monitor_info) {
  if (!monitor_info) return CollectStatus::kError;
  std::ifstream file("/proc/meminfo");
  if (!file.is_open()) return CollectStatus::kError;

  std::map<std::string, std::int64_t> values;
  std::string line;
  while (std::getline(file, line)) {
    std::istringstream iss(line);
    std::string name;
    std::string value_token;
    std::string unit;
    std::int64_t value = 0;
    if (!(iss >> name >> value_token) ||
        name.empty() || name.back() != ':' ||
        !ParseNonnegative(value_token, &value)) {
      return CollectStatus::kError;
    }
    if (iss >> unit && unit != "kB") return CollectStatus::kError;
    std::string extra;
    if (iss >> extra) return CollectStatus::kError;
    if (!name.empty() && name.back() == ':') name.pop_back();
    values[name] = value;
  }
  if (file.bad() || !values.count("MemTotal") ||
      !values.count("MemAvailable") || values["MemTotal"] == 0) {
    return CollectStatus::kError;
  }

  const double total_kib = static_cast<double>(values["MemTotal"]);
  const double available_kib = static_cast<double>(values["MemAvailable"]);
  const double used_percent =
      (total_kib - available_kib) / total_kib * 100.0;
  if (!std::isfinite(used_percent) || used_percent < 0.0 ||
      used_percent > 100.0) {
    return CollectStatus::kError;
  }
  const auto value = [&values](const char* name) {
    const auto it = values.find(name);
    return it == values.end() ? 0.0 : static_cast<double>(it->second) /
                                          (1024.0 * 1024.0);
  };

  auto* memory = monitor_info->mutable_mem_info();
  memory->set_used_percent(used_percent);
  memory->set_total(value("MemTotal"));
  memory->set_free(value("MemFree"));
  memory->set_avail(value("MemAvailable"));
  memory->set_buffers(value("Buffers"));
  memory->set_cached(value("Cached"));
  memory->set_swap_cached(value("SwapCached"));
  memory->set_active(value("Active"));
  memory->set_inactive(value("Inactive"));
  memory->set_active_anon(value("Active(anon)"));
  memory->set_inactive_anon(value("Inactive(anon)"));
  memory->set_active_file(value("Active(file)"));
  memory->set_inactive_file(value("Inactive(file)"));
  memory->set_dirty(value("Dirty"));
  memory->set_writeback(value("Writeback"));
  memory->set_anon_pages(value("AnonPages"));
  memory->set_mapped(value("Mapped"));
  memory->set_kreclaimable(value("KReclaimable"));
  memory->set_sreclaimable(value("SReclaimable"));
  memory->set_sunreclaim(value("SUnreclaim"));
  memory->set_sample_valid(true);
  return CollectStatus::kOk;
}

}  // namespace monitor
