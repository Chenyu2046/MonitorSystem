#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <unordered_map>

#include "monitor/monitor_inter.h"
#include "monitor_info.pb.h"

namespace monitor {

struct DiskRawSample {
  std::string name;
  std::uint64_t reads = 0;
  std::uint64_t sectors_read = 0;
  std::uint64_t read_time_ms = 0;
  std::uint64_t writes = 0;
  std::uint64_t sectors_written = 0;
  std::uint64_t write_time_ms = 0;
  std::uint64_t io_in_progress = 0;
  std::uint64_t io_time_ms = 0;
  std::uint64_t weighted_io_time_ms = 0;
};

bool ParseDiskStatsLine(const std::string& line, DiskRawSample* out);

class DiskMonitor : public MonitorInter {
 public:
  bool Init() override;
  CollectStatus UpdateOnce(monitor::proto::MonitorInfo* monitor_info) override;
  void Stop() override {}

 private:
  struct CachedSample {
    DiskRawSample sample;
    std::chrono::steady_clock::time_point timestamp;
  };
  std::unordered_map<std::string, CachedSample> last_samples_;
};

}  // namespace monitor
