#include "monitor/disk_monitor.h"

#include <charconv>
#include <chrono>
#include <fstream>
#include <string_view>
#include <sstream>
#include <utility>
#include <vector>

namespace monitor {

namespace {
bool ParseUnsigned(std::string_view token, std::uint64_t* value) {
  if (!value || token.empty()) return false;
  const auto result = std::from_chars(token.data(), token.data() + token.size(),
                                      *value);
  return result.ec == std::errc() && result.ptr == token.data() + token.size();
}
}  // namespace

bool DiskMonitor::Init() {
  std::ifstream file("/proc/diskstats");
  return file.good();
}

bool ParseDiskStatsLine(const std::string& line, DiskRawSample* out) {
  if (!out) return false;
  std::istringstream iss(line);
  std::string major_token;
  std::string minor_token;
  std::string name;
  DiskRawSample sample;
  if (!(iss >> major_token >> minor_token >> name)) return false;
  std::uint64_t ignored = 0;
  if (!ParseUnsigned(major_token, &ignored) ||
      !ParseUnsigned(minor_token, &ignored) || name.empty()) return false;

  std::vector<std::uint64_t> fields;
  std::string token;
  while (iss >> token) {
    std::uint64_t value = 0;
    if (!ParseUnsigned(token, &value)) return false;
    fields.push_back(value);
  }
  if (fields.size() < 11) return false;
  sample.name = std::move(name);
  sample.reads = fields[0];
  sample.sectors_read = fields[2];
  sample.read_time_ms = fields[3];
  sample.writes = fields[4];
  sample.sectors_written = fields[6];
  sample.write_time_ms = fields[7];
  sample.io_in_progress = fields[8];
  sample.io_time_ms = fields[9];
  sample.weighted_io_time_ms = fields[10];
  *out = std::move(sample);
  return true;
}

CollectStatus DiskMonitor::UpdateOnce(
    monitor::proto::MonitorInfo* monitor_info) {
  if (!monitor_info) return CollectStatus::kError;
  std::ifstream file("/proc/diskstats");
  if (!file.is_open()) return CollectStatus::kError;

  const auto now = std::chrono::steady_clock::now();
  std::vector<DiskRawSample> current;
  std::string line;
  while (std::getline(file, line)) {
    if (line.empty()) continue;
    DiskRawSample sample;
    if (!ParseDiskStatsLine(line, &sample)) return CollectStatus::kError;
    if (sample.name.find("loop") == 0 || sample.name.find("ram") == 0) {
      continue;
    }
    current.push_back(std::move(sample));
  }
  if (file.bad()) return CollectStatus::kError;

  bool not_ready = current.empty();
  std::vector<std::pair<DiskRawSample, CachedSample>> report;
  report.reserve(current.size());
  for (const auto& sample : current) {
    const auto it = last_samples_.find(sample.name);
    if (it == last_samples_.end()) {
      not_ready = true;
      continue;
    }
    const auto& previous = it->second.sample;
    if (sample.reads < previous.reads ||
        sample.sectors_read < previous.sectors_read ||
        sample.read_time_ms < previous.read_time_ms ||
        sample.writes < previous.writes ||
        sample.sectors_written < previous.sectors_written ||
        sample.write_time_ms < previous.write_time_ms ||
        sample.io_time_ms < previous.io_time_ms ||
        sample.weighted_io_time_ms < previous.weighted_io_time_ms ||
        std::chrono::duration<double>(now - it->second.timestamp).count() <=
            0) {
      not_ready = true;
    }
    report.emplace_back(sample, it->second);
  }
  for (const auto& sample : current) {
    last_samples_[sample.name] = {sample, now};
  }
  if (not_ready) return CollectStatus::kNotReady;

  for (const auto& [sample, previous] : report) {
    const double dt =
        std::chrono::duration<double>(now - previous.timestamp).count();
    const double read_ios =
        static_cast<double>(sample.reads - previous.sample.reads);
    const double write_ios =
        static_cast<double>(sample.writes - previous.sample.writes);
    const double read_bytes = static_cast<double>(
        sample.sectors_read - previous.sample.sectors_read) * 512.0;
    const double write_bytes = static_cast<double>(
        sample.sectors_written - previous.sample.sectors_written) * 512.0;
    const double read_time = static_cast<double>(
        sample.read_time_ms - previous.sample.read_time_ms);
    const double write_time = static_cast<double>(
        sample.write_time_ms - previous.sample.write_time_ms);
    const double io_time = static_cast<double>(
        sample.io_time_ms - previous.sample.io_time_ms);

    auto* disk = monitor_info->add_disk_info();
    disk->set_name(sample.name);
    disk->set_reads(sample.reads);
    disk->set_writes(sample.writes);
    disk->set_sectors_read(sample.sectors_read);
    disk->set_sectors_written(sample.sectors_written);
    disk->set_read_time_ms(sample.read_time_ms);
    disk->set_write_time_ms(sample.write_time_ms);
    disk->set_io_in_progress(sample.io_in_progress);
    disk->set_io_time_ms(sample.io_time_ms);
    disk->set_weighted_io_time_ms(sample.weighted_io_time_ms);
    disk->set_read_bytes_per_sec(read_bytes / dt);
    disk->set_write_bytes_per_sec(write_bytes / dt);
    disk->set_read_iops(read_ios / dt);
    disk->set_write_iops(write_ios / dt);
    disk->set_avg_read_latency_ms(read_ios > 0 ? read_time / read_ios : 0);
    disk->set_avg_write_latency_ms(write_ios > 0 ? write_time / write_ios : 0);
    disk->set_util_percent(io_time / (dt * 1000.0) * 100.0);
  }
  return CollectStatus::kOk;
}

}  // namespace monitor
