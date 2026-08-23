#include "monitor/cpu_softirq_monitor.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <chrono>
#include <cstring>
#include <utility>
#include <vector>

#include "monitor/monitor_structs.h"

namespace monitor {

namespace {
constexpr const char* kDevicePath = "/dev/cpu_softirq_monitor";
constexpr std::size_t kMaxCpus = 256;
}

bool CpuSoftIrqMonitor::Init() {
  const int fd = open(kDevicePath, O_RDONLY);
  if (fd < 0) return false;
  const std::size_t size = sizeof(softirq_stat) * kMaxCpus;
  void* address = mmap(nullptr, size, PROT_READ, MAP_SHARED, fd, 0);
  const bool valid = address != MAP_FAILED;
  if (valid) munmap(address, size);
  close(fd);
  return valid;
}

CollectStatus CpuSoftIrqMonitor::UpdateOnce(
    monitor::proto::MonitorInfo* monitor_info) {
  if (!monitor_info) return CollectStatus::kError;
  const int fd = open(kDevicePath, O_RDONLY);
  if (fd < 0) return CollectStatus::kError;
  const std::size_t size = sizeof(softirq_stat) * kMaxCpus;
  void* address = mmap(nullptr, size, PROT_READ, MAP_SHARED, fd, 0);
  if (address == MAP_FAILED) {
    close(fd);
    return CollectStatus::kError;
  }

  const auto* mapped = static_cast<const softirq_stat*>(address);
  struct Raw {
    std::string name;
    std::uint64_t values[10]{};
  };
  std::vector<Raw> current;
  for (std::size_t i = 0; i < kMaxCpus && mapped[i].cpu_name[0] != '\0'; ++i) {
    Raw raw;
    raw.name = mapped[i].cpu_name;
    raw.values[0] = mapped[i].hi;
    raw.values[1] = mapped[i].timer;
    raw.values[2] = mapped[i].net_tx;
    raw.values[3] = mapped[i].net_rx;
    raw.values[4] = mapped[i].block;
    raw.values[5] = mapped[i].irq_poll;
    raw.values[6] = mapped[i].tasklet;
    raw.values[7] = mapped[i].sched;
    raw.values[8] = mapped[i].hrtimer;
    raw.values[9] = mapped[i].rcu;
    current.push_back(std::move(raw));
  }
  munmap(address, size);
  close(fd);
  if (current.empty()) return CollectStatus::kError;

  const auto now = std::chrono::steady_clock::now();
  bool not_ready = false;
  struct Report {
    Raw current;
    SoftIrq previous;
  };
  std::vector<Report> report;
  report.reserve(current.size());
  for (const auto& raw : current) {
    const auto it = cpu_softirqs_.find(raw.name);
    if (it == cpu_softirqs_.end()) {
      not_ready = true;
      continue;
    }
    const std::uint64_t previous[10] = {
        it->second.hi,     it->second.timer,  it->second.net_tx,
        it->second.net_rx, it->second.block,  it->second.irq_poll,
        it->second.tasklet, it->second.sched, it->second.hrtimer,
        it->second.rcu};
    bool changed = false;
    for (std::size_t i = 0; i < 10; ++i) {
      if (raw.values[i] < previous[i]) not_ready = true;
      if (raw.values[i] > previous[i]) changed = true;
    }
    if (!changed) not_ready = true;
    if (std::chrono::duration<double>(now - it->second.timepoint).count() <=
        0) {
      not_ready = true;
    }
    report.push_back({raw, it->second});
  }

  for (const auto& raw : current) {
    SoftIrq& cached = cpu_softirqs_[raw.name];
    cached.cpu_name = raw.name;
    cached.hi = raw.values[0];
    cached.timer = raw.values[1];
    cached.net_tx = raw.values[2];
    cached.net_rx = raw.values[3];
    cached.block = raw.values[4];
    cached.irq_poll = raw.values[5];
    cached.tasklet = raw.values[6];
    cached.sched = raw.values[7];
    cached.hrtimer = raw.values[8];
    cached.rcu = raw.values[9];
    cached.timepoint = now;
  }
  if (not_ready) return CollectStatus::kNotReady;

  for (const auto& item : report) {
    const double seconds = std::chrono::duration<double>(
                               now - item.previous.timepoint)
                               .count();
    if (seconds <= 0) return CollectStatus::kError;
    auto* message = monitor_info->add_soft_irq();
    message->set_cpu(item.current.name);
    const auto rate = [&item, seconds](std::size_t index,
                                       std::uint64_t previous) {
      return static_cast<double>(item.current.values[index] - previous) /
             seconds;
    };
    message->set_hi(rate(0, item.previous.hi));
    message->set_timer(rate(1, item.previous.timer));
    message->set_net_tx(rate(2, item.previous.net_tx));
    message->set_net_rx(rate(3, item.previous.net_rx));
    message->set_block(rate(4, item.previous.block));
    message->set_irq_poll(rate(5, item.previous.irq_poll));
    message->set_tasklet(rate(6, item.previous.tasklet));
    message->set_sched(rate(7, item.previous.sched));
    message->set_hrtimer(rate(8, item.previous.hrtimer));
    message->set_rcu(rate(9, item.previous.rcu));
  }
  return CollectStatus::kOk;
}

}  // namespace monitor
