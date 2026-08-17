#include "diagnostics/anomaly_detector.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace monitor::diagnostics {
namespace {

double Normalize(double value, double warning, double critical) {
  if (critical <= warning) {
    return 0.0;
  }
  return std::clamp((value - warning) / (critical - warning), 0.0, 1.0);
}

void AddSignal(AnomalyResult* result, AnomalyDomain domain, const char* metric,
               double value, double score) {
  result->signals.push_back(
      AnomalySignal{domain, metric, value, score, score >= 0.5});
  result->overall_score = std::max(result->overall_score, score);
}

}  // namespace

AnomalyDetector::AnomalyDetector(ObservabilityConfig config)
    : config_(std::move(config)) {}

AnomalyResult AnomalyDetector::Evaluate(
    const monitor::proto::MonitorInfo& info) const {
  AnomalyResult result;

  double max_cpu = 0.0;
  double max_io_wait = 0.0;
  double max_softirq = 0.0;
  for (const auto& cpu : info.cpu_stat()) {
    max_cpu = std::max(max_cpu, static_cast<double>(cpu.cpu_percent()));
    max_io_wait =
        std::max(max_io_wait, static_cast<double>(cpu.io_wait_percent()));
    max_softirq =
        std::max(max_softirq, static_cast<double>(cpu.soft_irq_percent()));
  }
  if (info.cpu_stat_size() > 0) {
    AddSignal(&result, AnomalyDomain::kCpu, "cpu_percent", max_cpu,
              Normalize(max_cpu, config_.cpu_warning_percent,
                        config_.cpu_critical_percent));
    AddSignal(&result, AnomalyDomain::kCpu, "io_wait_percent", max_io_wait,
              Normalize(max_io_wait, config_.io_wait_warning_percent,
                        config_.io_wait_critical_percent));
    AddSignal(&result, AnomalyDomain::kCpu, "softirq_percent", max_softirq,
              Normalize(max_softirq, config_.softirq_warning_percent,
                        config_.softirq_critical_percent));
  }

  if (info.has_cpu_load()) {
    const double load = info.cpu_load().load_avg_1();
    AddSignal(&result, AnomalyDomain::kScheduler, "load_avg_1", load,
              Normalize(load, config_.load_warning, config_.load_critical));
  }

  if (info.has_mem_info()) {
    const double used = info.mem_info().used_percent();
    AddSignal(&result, AnomalyDomain::kMemory, "memory_used_percent", used,
              Normalize(used, config_.memory_warning_percent,
                        config_.memory_critical_percent));
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
    AddSignal(&result, AnomalyDomain::kDisk, "disk_util_percent", max_disk_util,
              Normalize(max_disk_util, config_.disk_util_warning_percent,
                        config_.disk_util_critical_percent));
    AddSignal(&result, AnomalyDomain::kDisk, "disk_latency_ms",
              max_disk_latency,
              Normalize(max_disk_latency, config_.disk_latency_warning_ms,
                        config_.disk_latency_critical_ms));
  }

  double packets_per_second = 0.0;
  for (const auto& net : info.net_info()) {
    packets_per_second += net.rcv_packets_rate() + net.send_packets_rate();
  }
  if (info.net_info_size() > 0) {
    AddSignal(&result, AnomalyDomain::kNetwork, "packets_per_second",
              packets_per_second,
              Normalize(packets_per_second, config_.network_pps_warning,
                        config_.network_pps_critical));
  }

  double net_softirq_per_second = 0.0;
  for (const auto& softirq : info.soft_irq()) {
    net_softirq_per_second =
        std::max(net_softirq_per_second,
                 static_cast<double>(softirq.net_rx() + softirq.net_tx()));
  }
  if (info.soft_irq_size() > 0) {
    AddSignal(&result, AnomalyDomain::kNetwork, "net_softirq_per_second",
              net_softirq_per_second,
              Normalize(net_softirq_per_second, config_.softirq_warning_per_sec,
                        config_.softirq_critical_per_sec));
  }

  result.should_diagnose = result.overall_score >= config_.suspect_enter_score;
  result.should_profile = result.overall_score >= config_.profiling_enter_score;
  return result;
}

}  // namespace monitor::diagnostics
