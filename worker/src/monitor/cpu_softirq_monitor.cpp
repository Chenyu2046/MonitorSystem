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

namespace cpu_softirq_detail {

bool ComputeRates(const Counters& current, const Counters& previous,
                  double seconds, Rates* rates) {
  if (!rates || seconds <= 0) return false;
  for (std::size_t i = 0; i < current.size(); ++i) {
    if (current[i] < previous[i]) return false;
    (*rates)[i] = static_cast<double>(current[i] - previous[i]) / seconds;
  }
  return true;
}

}  // namespace cpu_softirq_detail

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
    cpu_softirq_detail::Counters values{};
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
    cpu_softirq_detail::Rates rates;
  };
  std::vector<Report> report;
  report.reserve(current.size());
  for (const auto& raw : current) {
    const auto it = cpu_softirqs_.find(raw.name);
    if (it == cpu_softirqs_.end()) {
      not_ready = true;
      continue;
    }
    const cpu_softirq_detail::Counters previous = {
        it->second.hi,     it->second.timer,  it->second.net_tx,
        it->second.net_rx, it->second.block,  it->second.irq_poll,
        it->second.tasklet, it->second.sched, it->second.hrtimer,
        it->second.rcu};
    cpu_softirq_detail::Rates rates{};
    const double seconds =
        std::chrono::duration<double>(now - it->second.timepoint).count();
    if (!cpu_softirq_detail::ComputeRates(raw.values, previous, seconds,
                                          &rates)) {
      not_ready = true;
    }
    report.push_back({raw, rates});
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
    auto* message = monitor_info->add_soft_irq();
    message->set_cpu(item.current.name);
    message->set_hi(item.rates[0]);
    message->set_timer(item.rates[1]);
    message->set_net_tx(item.rates[2]);
    message->set_net_rx(item.rates[3]);
    message->set_block(item.rates[4]);
    message->set_irq_poll(item.rates[5]);
    message->set_tasklet(item.rates[6]);
    message->set_sched(item.rates[7]);
    message->set_hrtimer(item.rates[8]);
    message->set_rcu(item.rates[9]);
    message->set_sample_valid(true);
  }
  return CollectStatus::kOk;
}

}  // namespace monitor
