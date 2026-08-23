#include "monitor/cpu_stat_monitor.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <vector>
#include <utility>

#include "monitor/monitor_structs.h"

namespace monitor {

namespace cpu_stat_detail {

CpuStatDelta ComputeCpuStatDelta(const CpuStatSnapshot& current,
                                 const CpuStatSnapshot& previous) {
  CpuStatDelta delta;
  delta.counter_reset =
      current.user < previous.user || current.nice < previous.nice ||
      current.system < previous.system || current.idle < previous.idle ||
      current.io_wait < previous.io_wait || current.irq < previous.irq ||
      current.soft_irq < previous.soft_irq || current.steal < previous.steal ||
      current.guest < previous.guest || current.guest_nice < previous.guest_nice;
  if (delta.counter_reset) return delta;

  delta.user = current.user - previous.user;
  delta.nice = current.nice - previous.nice;
  delta.system = current.system - previous.system;
  delta.idle = current.idle - previous.idle;
  delta.io_wait = current.io_wait - previous.io_wait;
  delta.irq = current.irq - previous.irq;
  delta.soft_irq = current.soft_irq - previous.soft_irq;
  delta.steal = current.steal - previous.steal;
  delta.total = delta.user + delta.nice + delta.system + delta.idle +
                delta.io_wait + delta.irq + delta.soft_irq + delta.steal;
  delta.busy = delta.user + delta.nice + delta.system + delta.irq +
               delta.soft_irq + delta.steal;
  return delta;
}

}  // namespace cpu_stat_detail

bool CpuStatMonitor::Init() {
  const long configured_cpus = sysconf(_SC_NPROCESSORS_CONF);
  if (configured_cpus > 256) return false;
  const int fd = open("/dev/cpu_stat_monitor", O_RDONLY);
  if (fd < 0) return false;
  const std::size_t size = sizeof(cpu_stat) * 256;
  void* address = mmap(nullptr, size, PROT_READ, MAP_SHARED, fd, 0);
  const bool valid = address != MAP_FAILED;
  if (valid) munmap(address, size);
  close(fd);
  return valid;
}

CollectStatus CpuStatMonitor::UpdateOnce(
    monitor::proto::MonitorInfo* monitor_info) {
  if (!monitor_info) return CollectStatus::kError;
  const int fd = open("/dev/cpu_stat_monitor", O_RDONLY);
  if (fd < 0) return CollectStatus::kError;
  constexpr std::size_t kMaxCpus = 256;
  const std::size_t size = sizeof(cpu_stat) * kMaxCpus;
  void* address = mmap(nullptr, size, PROT_READ, MAP_SHARED, fd, 0);
  if (address == MAP_FAILED) {
    close(fd);
    return CollectStatus::kError;
  }

  const auto* mapped = static_cast<const cpu_stat*>(address);
  struct Raw {
    std::string name;
    cpu_stat_detail::CpuStatSnapshot snapshot;
  };
  std::vector<Raw> current;
  for (std::size_t i = 0; i < kMaxCpus && mapped[i].cpu_name[0] != '\0'; ++i) {
    Raw raw;
    raw.name = mapped[i].cpu_name;
    raw.snapshot.user = mapped[i].user;
    raw.snapshot.nice = mapped[i].nice;
    raw.snapshot.system = mapped[i].system;
    raw.snapshot.idle = mapped[i].idle;
    raw.snapshot.io_wait = mapped[i].iowait;
    raw.snapshot.irq = mapped[i].irq;
    raw.snapshot.soft_irq = mapped[i].softirq;
    raw.snapshot.steal = mapped[i].steal;
    raw.snapshot.guest = mapped[i].guest;
    raw.snapshot.guest_nice = mapped[i].guest_nice;
    current.push_back(std::move(raw));
  }
  munmap(address, size);
  close(fd);
  if (current.empty()) return CollectStatus::kError;

  bool not_ready = false;
  struct Report {
    Raw current;
    cpu_stat_detail::CpuStatDelta delta;
  };
  std::vector<Report> report;
  report.reserve(current.size());
  for (const auto& raw : current) {
    const auto it = cpu_stat_map_.find(raw.name);
    if (it == cpu_stat_map_.end()) {
      not_ready = true;
      continue;
    }
    const auto delta = cpu_stat_detail::ComputeCpuStatDelta(
        raw.snapshot, it->second);
    if (!delta.IsReportable()) not_ready = true;
    report.push_back({raw, delta});
  }
  for (const auto& raw : current) {
    cpu_stat_map_[raw.name] = raw.snapshot;
  }
  if (not_ready) return CollectStatus::kNotReady;

  for (const auto& item : report) {
    const auto& delta = item.delta;
    auto* message = monitor_info->add_cpu_stat();
    const double total = static_cast<double>(delta.total);
    message->set_cpu_name(item.current.name);
    message->set_cpu_percent(delta.busy * 100.0 / total);
    message->set_usr_percent(delta.user * 100.0 / total);
    message->set_system_percent(delta.system * 100.0 / total);
    message->set_nice_percent(delta.nice * 100.0 / total);
    message->set_idle_percent(delta.idle * 100.0 / total);
    message->set_io_wait_percent(delta.io_wait * 100.0 / total);
    message->set_irq_percent(delta.irq * 100.0 / total);
    message->set_soft_irq_percent(delta.soft_irq * 100.0 / total);
    message->set_sample_valid(true);
  }
  return CollectStatus::kOk;
}

}  // namespace monitor
