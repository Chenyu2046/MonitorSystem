/**
 * @file evidence_builder.cpp
 * @brief 普通监控/诊断 protobuf 到规则证据的构建实现。
 *
 * 本文件只生成内存 Evidence：Severity 将 warning~critical 归一化到 0~1，
 * Add() 生成带时间和序号的稳定 ID；RootCauseEngine 再基于多证据组合推断
 * CPU saturation、磁盘/网络压力、内存压力或锁竞争。
 */

#include "diagnostics/evidence_builder.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <iomanip>
#include <sstream>
#include <string_view>

namespace monitor::diagnostics {
namespace {

/** @brief 将单个原始指标按 warning/critical 线性归一化为 0~1。 */
double Severity(double value, double warning, double critical) {
  if (critical <= warning) {
    return 0.0;
  }
  return std::clamp((value - warning) / (critical - warning), 0.0, 1.0);
}

/**
 * @brief 追加带 source/target/unit/detail 的证据并生成可追踪 ID。
 */
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

/**
 * @brief 把 top profile 的 PID/TID、样本和前几层 stack 压缩为 detail。
 *
 * detail 只提供可读证据摘要，不能把 samples 数量解释为精确 CPU 百分比。
 */
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

/** @brief 通过内核栈符号关键词识别可能的锁等待证据。 */
bool HasLockWaitStack(const monitor::proto::ProfileEntry& profile) {
  constexpr std::array<std::string_view, 8> kLockSymbols = {
      "futex",       "pthread_mutex", "mutex_lock", "rwsem",
      "rwlock",      "sem_wait",      "__lll_lock", "mutex"};
  for (const auto& frame : profile.kernel_stack()) {
    std::string symbol;
    symbol.reserve(frame.symbol().size());
    for (const unsigned char character : frame.symbol()) {
      symbol.push_back(static_cast<char>(std::tolower(character)));
    }
    if (std::any_of(kLockSymbols.begin(), kLockSymbols.end(),
                    [&symbol](std::string_view keyword) {
                      return symbol.find(keyword) != std::string::npos;
                    })) {
      return true;
    }
  }
  return false;
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
    case EvidenceType::kDiagnosticCapabilityDegraded:
      return "diagnostic_capability_degraded";
    case EvidenceType::kMemoryAvailable:
      return "memory_available";
    case EvidenceType::kOnCpuStack:
      return "oncpu_stack";
    case EvidenceType::kOffCpuStack:
      return "offcpu_stack";
    case EvidenceType::kLockWaitStack:
      return "lock_wait_stack";
  }
  return "unknown";
}

std::vector<Evidence> EvidenceBuilder::Build(
    const monitor::proto::MonitorInfo& info,
    std::chrono::system_clock::time_point timestamp) const {
  // EvidenceBuilder 不保存跨轮状态；它只解释当前 MonitorInfo，并把
  // 逐核/逐设备 max 与网络总包速率转换为本轮可组合证据。
  std::vector<Evidence> evidence;

  // 异常证据采用最大核而非主机平均值，避免单核热点被稀释。
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

  // 磁盘证据采用最忙设备/最高延迟，保持热点设备的可见性。
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

  // 网络包速率按接口求和，描述主机整体网络栈处理压力。
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
        net_rx_softirq, "events/s", Severity(net_rx_softirq, 10000.0, 100000.0),
        timestamp, "max NET_RX softirq");
  }

  if (info.has_mem_info()) {
    const double available = info.mem_info().avail();
    const double used_percent = info.mem_info().used_percent();
    Add(&evidence, EvidenceType::kMemoryAvailable, "MonitorInfo.mem_info",
        available, "GiB", Severity(used_percent, 80.0, 95.0), timestamp,
        "available memory proxy from used percent");
  }

  // DiagnosticSnapshot 是另一条异常诊断路径；普通 MonitorInfo 缺少
  // 这些进程/请求级证据时，构建器不会虚构对应 evidence。
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
    for (const auto& status : info.diagnostic().probe_status()) {
      if (status.requested() &&
          (!status.available() || !status.attached() ||
           !status.snapshot_ok())) {
        const bool read_failed = status.available() && status.attached() &&
                                 !status.snapshot_ok();
        Add(&evidence, EvidenceType::kDiagnosticCapabilityDegraded,
            "DiagnosticSnapshot.probe_status",
            static_cast<double>(status.last_error()), "errno", 0.0, timestamp,
            read_failed ? "requested probe snapshot read failed: " +
                              status.probe()
                        : "requested probe unavailable: " + status.probe(),
            status.probe());
      }
    }
    if (info.diagnostic().oncpu_profiles_size() > 0) {
      double samples = 0.0;
      std::string detail = "top On-CPU profile entries";
      bool has_stack = false;
      for (const auto& profile : info.diagnostic().oncpu_profiles()) {
        if (profile.user_stack_size() == 0 &&
            profile.kernel_stack_size() == 0) {
          continue;
        }
        samples += profile.samples();
        if (!has_stack) {
          detail += "; " + StackSummary(profile, true);
          has_stack = true;
        }
      }
      if (has_stack) {
        Add(&evidence, EvidenceType::kOnCpuStack, "DiagnosticSnapshot",
            samples, "samples", 1.0, timestamp, detail);
      }
    }
    if (info.diagnostic().offcpu_profiles_size() > 0) {
      double duration_ns = 0.0;
      std::string detail = "top Off-CPU profile entries";
      std::string lock_detail;
      double lock_duration_ns = 0.0;
      bool has_stack = false;
      for (const auto& profile : info.diagnostic().offcpu_profiles()) {
        if (profile.kernel_stack_size() == 0) {
          continue;
        }
        duration_ns += profile.total_offcpu_ns();
        if (!has_stack) {
          detail += "; " + StackSummary(profile, false);
          has_stack = true;
        }
        if (HasLockWaitStack(profile)) {
          lock_duration_ns += profile.total_offcpu_ns();
          if (lock_detail.empty()) {
            lock_detail = "lock-wait stack; " + StackSummary(profile, false);
          }
        }
      }
      if (has_stack) {
        Add(&evidence, EvidenceType::kOffCpuStack, "DiagnosticSnapshot",
            duration_ns, "ns", 1.0, timestamp, detail);
      }
      if (!lock_detail.empty()) {
        Add(&evidence, EvidenceType::kLockWaitStack, "DiagnosticSnapshot",
            lock_duration_ns, "ns", 1.0, timestamp, lock_detail);
      }
    }
  }
  return evidence;
}

}  // namespace monitor::diagnostics
