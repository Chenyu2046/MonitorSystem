#include "diagnostics/evidence_builder.h"

#include <algorithm>
#include <iomanip>
#include <sstream>

namespace monitor::diagnostics {
namespace {

double Severity(double value, double warning, double critical) {
  if (critical <= warning) {
    return 0.0;
  }
  return std::clamp((value - warning) / (critical - warning), 0.0, 1.0);
}

void Add(std::vector<Evidence>* evidence, EvidenceType type,
         const std::string& source, double value, const std::string& unit,
         double severity, std::chrono::system_clock::time_point timestamp,
         const std::string& detail, const std::string& target = "host") {
  const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(
                          timestamp.time_since_epoch())
                          .count();
  const std::string resolved_target = target.empty() ? "host" : target;
  evidence->push_back(Evidence{
      std::string(EvidenceTypeName(type)) + ":" + std::to_string(millis) + ":" +
          std::to_string(evidence->size()),
      type, source, resolved_target, value, unit, severity, timestamp, detail});
}

std::string StackSummary(const monitor::proto::ProfileEntry& profile,
                         bool include_user_stack) {
  std::ostringstream detail;
  detail << "pid=" << profile.pid() << " tid=" << profile.tid()
         << " samples=" << profile.samples()
         << " offcpu_ns=" << profile.total_offcpu_ns();
  const auto append_frames = [&detail](const char* label,
                                        const auto& frames) {
    detail << " " << label << "=[";
    constexpr int kMaxSummaryFrames = 5;
    for (int index = 0;
         index < std::min(frames.size(), kMaxSummaryFrames); ++index) {
      if (index != 0) {
        detail << ",";
      }
      const auto& frame = frames.Get(index);
      if (frame.symbol().empty()) {
        detail << "0x" << std::hex << frame.address() << std::dec;
      } else {
        detail << frame.symbol();
      }
    }
    if (frames.size() > kMaxSummaryFrames) {
      detail << ",...";
    }
    detail << "]";
  };
  if (include_user_stack) {
    append_frames("user", profile.user_stack());
  }
  append_frames("kernel", profile.kernel_stack());
  return detail.str();
}

}  // namespace

const char* EvidenceTypeName(EvidenceType type) {
  switch (type) {
    case EvidenceType::kCpuUsage:
      return "cpu_usage";
    case EvidenceType::kRunQueue:
      return "run_queue";
    case EvidenceType::kIoWait:
      return "io_wait";
    case EvidenceType::kDiskUtil:
      return "disk_util";
    case EvidenceType::kDiskLatency:
      return "disk_latency";
    case EvidenceType::kBpfBlockLatency:
      return "bpf_block_latency";
    case EvidenceType::kNetPps:
      return "net_pps";
    case EvidenceType::kTcpRetrans:
      return "tcp_retrans";
    case EvidenceType::kSoftirqNetRx:
      return "softirq_net_rx";
    case EvidenceType::kSchedulerSwitches:
      return "scheduler_switches";
    case EvidenceType::kSchedulerWakeups:
      return "scheduler_wakeups";
    case EvidenceType::kMemoryAvailable:
      return "memory_available";
    case EvidenceType::kOnCpuStack:
      return "oncpu_stack";
    case EvidenceType::kOffCpuStack:
      return "offcpu_stack";
  }
  return "unknown";
}

std::vector<Evidence> EvidenceBuilder::Build(
    const monitor::proto::MonitorInfo& info,
    std::chrono::system_clock::time_point timestamp) const {
  std::vector<Evidence> evidence;

  double max_cpu = 0.0;
  double max_io_wait = 0.0;
  for (const auto& cpu : info.cpu_stat()) {
    max_cpu = std::max(max_cpu, static_cast<double>(cpu.cpu_percent()));
    max_io_wait =
        std::max(max_io_wait, static_cast<double>(cpu.io_wait_percent()));
  }
  if (info.cpu_stat_size() > 0) {
    Add(&evidence, EvidenceType::kCpuUsage, "MonitorInfo.cpu_stat", max_cpu,
        "%", Severity(max_cpu, 70.0, 90.0), timestamp, "max CPU usage");
    Add(&evidence, EvidenceType::kIoWait, "MonitorInfo.cpu_stat", max_io_wait,
        "%", Severity(max_io_wait, 10.0, 30.0), timestamp, "max IOWait");
  }

  if (info.has_cpu_load()) {
    const double load = info.cpu_load().load_avg_1();
    Add(&evidence, EvidenceType::kRunQueue, "MonitorInfo.cpu_load", load,
        "load", Severity(load, 1.0, 4.0), timestamp, "load average 1m");
  }

  double max_disk_util = 0.0;
  double max_disk_latency = 0.0;
  for (const auto& disk : info.disk_info()) {
    max_disk_util = std::max(max_disk_util, disk.util_percent());
    max_disk_latency = std::max(
        max_disk_latency,
        std::max(disk.avg_read_latency_ms(), disk.avg_write_latency_ms()));
  }
  if (info.disk_info_size() > 0) {
    Add(&evidence, EvidenceType::kDiskUtil, "MonitorInfo.disk_info",
        max_disk_util, "%", Severity(max_disk_util, 70.0, 95.0), timestamp,
        "max disk utilization");
    Add(&evidence, EvidenceType::kDiskLatency, "MonitorInfo.disk_info",
        max_disk_latency, "ms", Severity(max_disk_latency, 10.0, 40.0),
        timestamp, "max disk latency");
  }

  double pps = 0.0;
  for (const auto& net : info.net_info()) {
    pps += net.rcv_packets_rate() + net.send_packets_rate();
  }
  if (info.net_info_size() > 0) {
    Add(&evidence, EvidenceType::kNetPps, "MonitorInfo.net_info", pps, "pkt/s",
        Severity(pps, 10000.0, 100000.0), timestamp, "aggregate packet rate");
  }

  double net_rx_softirq = 0.0;
  for (const auto& softirq : info.soft_irq()) {
    net_rx_softirq =
        std::max(net_rx_softirq, static_cast<double>(softirq.net_rx()));
  }
  if (info.soft_irq_size() > 0) {
    Add(&evidence, EvidenceType::kSoftirqNetRx, "MonitorInfo.soft_irq",
        net_rx_softirq, "count/s", Severity(net_rx_softirq, 10000.0, 100000.0),
        timestamp, "max NET_RX softirq");
  }

  if (info.has_mem_info()) {
    const double available = info.mem_info().avail();
    const double used_percent = info.mem_info().used_percent();
    Add(&evidence, EvidenceType::kMemoryAvailable, "MonitorInfo.mem_info",
        available, "MB", Severity(used_percent, 80.0, 95.0), timestamp,
        "available memory proxy from used percent");
  }

  if (info.has_diagnostic()) {
    for (const auto& signal : info.diagnostic().signals()) {
      if (signal.metric() == "tcp_retransmissions") {
        Add(&evidence, EvidenceType::kTcpRetrans, "DiagnosticSnapshot",
            signal.value(), signal.unit(), signal.anomaly_score(), timestamp,
            "TCP retransmission aggregate", signal.target());
      } else if (signal.metric() == "block_io_avg_latency_ms") {
        Add(&evidence, EvidenceType::kBpfBlockLatency,
            "DiagnosticSnapshot.block_io", signal.value(), signal.unit(),
            std::max(signal.anomaly_score(),
                     Severity(signal.value(), 10.0, 40.0)),
            timestamp, "eBPF block I/O latency", signal.target());
      } else if (signal.metric() == "scheduler_switches") {
        Add(&evidence, EvidenceType::kSchedulerSwitches,
            "DiagnosticSnapshot.scheduler", signal.value(), signal.unit(),
            signal.anomaly_score(), timestamp, "scheduler context switches",
            signal.target());
      } else if (signal.metric() == "scheduler_wakeups") {
        Add(&evidence, EvidenceType::kSchedulerWakeups,
            "DiagnosticSnapshot.scheduler", signal.value(), signal.unit(),
            signal.anomaly_score(), timestamp, "scheduler wakeups",
            signal.target());
      }
    }
    if (info.diagnostic().oncpu_profiles_size() > 0) {
      double samples = 0.0;
      std::string detail = "top On-CPU profile entries";
      for (const auto& profile : info.diagnostic().oncpu_profiles()) {
        samples += profile.samples();
        if (detail == "top On-CPU profile entries") {
          detail += "; " + StackSummary(profile, true);
        }
      }
      Add(&evidence, EvidenceType::kOnCpuStack, "DiagnosticSnapshot", samples,
          "samples", 1.0, timestamp, detail);
    }
    if (info.diagnostic().offcpu_profiles_size() > 0) {
      double duration_ns = 0.0;
      std::string detail = "top Off-CPU profile entries";
      for (const auto& profile : info.diagnostic().offcpu_profiles()) {
        duration_ns += profile.total_offcpu_ns();
        if (detail == "top Off-CPU profile entries") {
          detail += "; " + StackSummary(profile, false);
        }
      }
      Add(&evidence, EvidenceType::kOffCpuStack, "DiagnosticSnapshot",
          duration_ns, "ns", 1.0, timestamp, detail);
    }
  }
  return evidence;
}

}  // namespace monitor::diagnostics
